#include "xr_session.hpp"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <vector>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "eyes.hpp"
#include "ipc.hpp"
#include "log.hpp"
#include "pose.hpp"
#include "shared_eyes.hpp"
#include "submit.hpp"
#include "vec_math.hpp"

namespace xrs {

namespace {

#define XR_TRY(call)                                                            \
    do {                                                                        \
        XrResult r_ = (call);                                                   \
        if (XR_FAILED(r_)) {                                                    \
            hostlog::write("openxr: %s failed: %d", #call, (int)r_);            \
            return false;                                                       \
        }                                                                       \
    } while (0)

struct Eye {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    uint32_t w = 0, h = 0;
    std::vector<XrSwapchainImageD3D11KHR> images;
    std::vector<ID3D11RenderTargetView*> rtvs;
};

struct State {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace space = XR_NULL_HANDLE;
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    uint32_t recenterCount = 0;
    uint32_t pubFrame = 0;
    bool running = false;
    bool exiting = false;
    // True when WE initiated the session end (carrier Shutdown / carrier
    // death / frame limit). Runtime-initiated terminations (EXITING /
    // LOSS_PENDING / instance loss without this flag) push MC2VR_MSG_EXIT so
    // the carrier quits the game cleanly (S4-5).
    bool selfExit = false;
    d3d::Device d3d;
    Eye eye[2];
    // S4-3 submission stats (10s window; the log is the acceptance evidence)
    uint64_t subLast[2] = {0, 0};  // last submitted carrier frameId per eye
    uint64_t subFresh = 0, subReused = 0, subPattern = 0;
    ULONGLONG subNext = 0;
    // S4-4: history of the published views, keyed by poseId (= hostFrame+1).
    // The carrier echoes the id of the pose it rendered with in FRAME_READY;
    // the projection layer is submitted with that pose+FOV so the compositor
    // reprojects from what the image actually contains.
    struct PoseRec { uint32_t id = 0; XrPosef pose[2]; XrFovf fov[2]; };
    static constexpr uint32_t kHist = 256;
    PoseRec hist[kHist];
    uint64_t poseHit = 0, poseMiss = 0, poseNone = 0;
};

// Distance between the two eye positions (the runtime's IPD), meters.
float eye_separation(const XrView views[2]) {
    auto pos = [](const XrView& v) {
        return mc2vr::math::Vec3{v.pose.position.x, v.pose.position.y, v.pose.position.z};
    };
    return mc2vr::math::distance(pos(views[0]), pos(views[1]));
}

const char* state_name(XrSessionState s) {
    switch (s) {
        case XR_SESSION_STATE_IDLE: return "IDLE";
        case XR_SESSION_STATE_READY: return "READY";
        case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
        case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
        case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
        case XR_SESSION_STATE_STOPPING: return "STOPPING";
        case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
        case XR_SESSION_STATE_EXITING: return "EXITING";
        default: return "UNKNOWN";
    }
}

// Proton's wineopenxr proxy expects HKCU\\Software\\Wine\\VR to exist; nothing
// creates it for a standalone (non-Steam-launched) process, so negotiation
// fails with -6. The DLL exports an initializer for exactly this.
void init_wine_vr_registry() {
    HKEY k;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\Wine\\VR", 0, nullptr, 0, KEY_ALL_ACCESS,
                        nullptr, &k, nullptr) == ERROR_SUCCESS) {
        // vrclient_x64 normally publishes the VR "state" (1 = runtime available);
        // without it wineopenxr negotiation fails (-6). Only fill it if absent.
        DWORD v = 0, sz = sizeof v, type = 0;
        if (RegQueryValueExA(k, "state", nullptr, &type, (BYTE*)&v, &sz) != ERROR_SUCCESS) {
            v = 1;
            RegSetValueExA(k, "state", 0, REG_DWORD, (const BYTE*)&v, sizeof v);
            hostlog::write("openxr: set Software\\Wine\\VR\\state=1 (was absent)");
        }
        RegCloseKey(k);
    }
    HMODULE m = LoadLibraryW(L"wineopenxr.dll");
    auto fn = m ? (void (*)())GetProcAddress(m, "wineopenxr_init_registry") : nullptr;
    hostlog::write("openxr: wineopenxr_init_registry %s", fn ? "called" : "not present (native runtime?)");
    if (fn) fn();
}

bool create_instance(State& s) {
    init_wine_vr_registry();
    uint32_t n = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
    std::vector<XrExtensionProperties> exts(n, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, n, &n, exts.data());
    bool haveD3D11 = false;
    for (auto& e : exts) {
        hostlog::write("openxr: extension %s v%u", e.extensionName, e.extensionVersion);
        if (!strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME)) haveD3D11 = true;
    }
    if (!haveD3D11) {
        hostlog::write("openxr: runtime lacks " XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
        return false;
    }

    const char* enable[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ci.applicationInfo.applicationName, "mc2vr_host");
    ci.applicationInfo.applicationVersion = 1;
    strcpy(ci.applicationInfo.engineName, "mc2vr");
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = enable;
    XR_TRY(xrCreateInstance(&ci, &s.instance));

    XrInstanceProperties ip = {XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(s.instance, &ip);
    hostlog::write("openxr: runtime '%s' %u.%u.%u", ip.runtimeName,
                   XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
                   XR_VERSION_PATCH(ip.runtimeVersion));

    XrSystemGetInfo gi = {XR_TYPE_SYSTEM_GET_INFO};
    gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_TRY(xrGetSystem(s.instance, &gi, &s.system));
    XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
    XR_TRY(xrGetSystemProperties(s.instance, s.system, &sp));
    hostlog::write("openxr: system '%s' maxSwapchain=%ux%u orientation=%d position=%d",
                   sp.systemName, sp.graphicsProperties.maxSwapchainImageWidth,
                   sp.graphicsProperties.maxSwapchainImageHeight, (int)sp.trackingProperties.orientationTracking,
                   (int)sp.trackingProperties.positionTracking);
    return true;
}

bool create_session(State& s) {
    PFN_xrGetD3D11GraphicsRequirementsKHR getReq = nullptr;
    XR_TRY(xrGetInstanceProcAddr(s.instance, "xrGetD3D11GraphicsRequirementsKHR",
                                 (PFN_xrVoidFunction*)&getReq));
    XrGraphicsRequirementsD3D11KHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    XR_TRY(getReq(s.instance, s.system, &req));
    hostlog::write("openxr: D3D11 requirements: min feature level 0x%x", req.minFeatureLevel);

    if (!d3d::create(&req.adapterLuid, req.minFeatureLevel, s.d3d)) return false;

    XrGraphicsBindingD3D11KHR gb = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    gb.device = s.d3d.dev;
    XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = s.system;
    XR_TRY(xrCreateSession(s.instance, &sci, &s.session));

    XrReferenceSpaceCreateInfo rsi = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsi.poseInReferenceSpace.orientation.w = 1;
    XR_TRY(xrCreateReferenceSpace(s.session, &rsi, &s.space));
    hostlog::write("openxr: session + LOCAL space created (D3D11 binding)");
    return true;
}

bool create_swapchains(State& s) {
    uint32_t nv = 0;
    XR_TRY(xrEnumerateViewConfigurationViews(s.instance, s.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                             0, &nv, nullptr));
    if (nv != 2) {
        hostlog::write("openxr: expected 2 stereo views, got %u", nv);
        return false;
    }
    XrViewConfigurationView vcv[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    XR_TRY(xrEnumerateViewConfigurationViews(s.instance, s.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                             2, &nv, vcv));

    uint32_t nf = 0;
    XR_TRY(xrEnumerateSwapchainFormats(s.session, 0, &nf, nullptr));
    std::vector<int64_t> fmts(nf);
    XR_TRY(xrEnumerateSwapchainFormats(s.session, nf, &nf, fmts.data()));
    // S4-3: the runtime offers sRGB-ONLY 8-bit formats (29/91, live-verified
    // S4-0 — no plain UNORM). The shared LDR finals are display-referred
    // sRGB-encoded bytes; the submit path renders them through UNORM-cast
    // views (SRV on the shared texture, RTV cast below) so the bytes pass
    // through untouched and the compositor decodes the sRGB swapchain — no
    // double gamma. Prefer the BGRA layout to match the source channels.
    int64_t chosen = fmts.empty() ? 0 : fmts[0];
    for (int64_t f : fmts) {
        hostlog::write("openxr: swapchain format %lld", (long long)f);
        if (f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
            chosen = f;
        else if (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
                 chosen != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
            chosen = f;
    }
    hostlog::write("openxr: using swapchain format %lld", (long long)chosen);
    // The blit writes raw bytes, so the RTV must NOT be the sRGB-typed one
    // (that would re-encode on write). Cast to the plain-UNORM sibling.
    DXGI_FORMAT rtvFmt = (chosen == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
                             ? DXGI_FORMAT_B8G8R8A8_UNORM
                         : (chosen == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
                             ? DXGI_FORMAT_R8G8B8A8_UNORM
                             : (DXGI_FORMAT)chosen;
    hostlog::write("openxr: swapchain RTV cast format %u", (unsigned)rtvFmt);

    for (int e = 0; e < 2; ++e) {
        Eye& ey = s.eye[e];
        ey.w = vcv[e].recommendedImageRectWidth;
        ey.h = vcv[e].recommendedImageRectHeight;
        XrSwapchainCreateInfo sc = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        sc.format = chosen;
        sc.sampleCount = 1;
        sc.width = ey.w;
        sc.height = ey.h;
        sc.faceCount = 1;
        sc.arraySize = 1;
        sc.mipCount = 1;
        XR_TRY(xrCreateSwapchain(s.session, &sc, &ey.swapchain));

        uint32_t ni = 0;
        XR_TRY(xrEnumerateSwapchainImages(ey.swapchain, 0, &ni, nullptr));
        ey.images.assign(ni, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        XR_TRY(xrEnumerateSwapchainImages(ey.swapchain, ni, &ni, (XrSwapchainImageBaseHeader*)ey.images.data()));
        for (auto& img : ey.images) {
            ID3D11RenderTargetView* rtv = nullptr;
            D3D11_RENDER_TARGET_VIEW_DESC rd = {};
            rd.Format = rtvFmt;
            rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            if (FAILED(s.d3d.dev->CreateRenderTargetView(img.texture, &rd, &rtv))) {
                hostlog::write("openxr: RTV creation failed (eye %d)", e);
                return false;
            }
            ey.rtvs.push_back(rtv);
        }
        hostlog::write("openxr: eye %d swapchain %ux%u, %u images", e, ey.w, ey.h, ni);
    }
    return true;
}

void handle_events(State& s) {
    XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(s.instance, &ev) == XR_SUCCESS) {
        switch (ev.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                auto* e = (XrEventDataSessionStateChanged*)&ev;
                s.sessionState = e->state;
                hostlog::write("openxr: session state -> %s", state_name(e->state));
                ipc::push_event(MC2VR_MSG_SESSION_STATE, e->state, s.recenterCount);
                if (e->state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    if (XR_SUCCEEDED(xrBeginSession(s.session, &bi))) s.running = true;
                } else if (e->state == XR_SESSION_STATE_STOPPING) {
                    xrEndSession(s.session);
                    s.running = false;
                } else if (e->state == XR_SESSION_STATE_EXITING ||
                           e->state == XR_SESSION_STATE_LOSS_PENDING) {
                    // Runtime-initiated end (SteamVR quitting, session lost):
                    // tell the carrier to quit the game cleanly (S4-5). When
                    // WE requested the exit (selfExit), the game is already
                    // on its way out — no event needed.
                    if (!s.selfExit) {
                        hostlog::write("openxr: runtime is ending the session — "
                                      "telling carrier to quit the game");
                        ipc::push_event(MC2VR_MSG_EXIT, (uint32_t)e->state, 0);
                    }
                    s.exiting = true;
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                hostlog::write("openxr: instance loss pending");
                if (!s.selfExit) {
                    hostlog::write("openxr: runtime is going away — telling "
                                  "carrier to quit the game");
                    ipc::push_event(MC2VR_MSG_EXIT, 0, 0);
                }
                s.exiting = true;
                break;
            case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
                hostlog::write("openxr: reference space change pending");
                ++s.recenterCount;
                ipc::push_event(MC2VR_MSG_RECENTER, s.recenterCount, 0);
                break;
            case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
                hostlog::write("openxr: interaction profile changed");
                break;
            default:
                hostlog::write("openxr: event type %d", (int)ev.type);
                break;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// One frame: wait/begin/locate/render/end. Returns false on a hard error.
void publish_frame(State& s, const XrFrameState& fs, const XrView views[2],
                   bool poseOk);

bool frame(State& s, unsigned n) {
    XrFrameState fs = {XR_TYPE_FRAME_STATE};
    XR_TRY(xrWaitFrame(s.session, nullptr, &fs));
    XR_TRY(xrBeginFrame(s.session, nullptr));

    XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                              {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const XrCompositionLayerBaseHeader* layers[1] = {(XrCompositionLayerBaseHeader*)&layer};
    uint32_t layerCount = 0;
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    bool poseOk = false;

    if (fs.shouldRender) {
        XrViewLocateInfo li = {XR_TYPE_VIEW_LOCATE_INFO};
        li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        li.displayTime = fs.predictedDisplayTime;
        li.space = s.space;
        XrViewState vs = {XR_TYPE_VIEW_STATE};
        uint32_t nv = 0;
        XR_TRY(xrLocateViews(s.session, &li, &vs, 2, &nv, views));

        poseOk = (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0 &&
                 (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
        if (n % 90 == 0) {
            hostlog::write("openxr: frame %u valid=%d L=(%.3f %.3f %.3f) q=(%.3f %.3f %.3f %.3f) "
                           "ipd=%.4f fovL=(%.3f %.3f %.3f %.3f)",
                           n, poseOk, views[0].pose.position.x, views[0].pose.position.y,
                           views[0].pose.position.z, views[0].pose.orientation.x,
                           views[0].pose.orientation.y, views[0].pose.orientation.z,
                           views[0].pose.orientation.w, eye_separation(views),
                           views[0].fov.angleLeft, views[0].fov.angleRight, views[0].fov.angleUp,
                           views[0].fov.angleDown);
        }

        // Layer pose/FOV (separate from `views`, which is what gets published).
        XrPosef lpose[2] = {views[0].pose, views[1].pose};
        XrFovf lfov[2] = {views[0].fov, views[1].fov};
        for (int e = 0; e < 2; ++e) {
            Eye& ey = s.eye[e];
            uint32_t idx = 0;
            XR_TRY(xrAcquireSwapchainImage(ey.swapchain, nullptr, &idx));
            XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wi.timeout = XR_INFINITE_DURATION;
            XR_TRY(xrWaitSwapchainImage(ey.swapchain, &wi));
            // S4-3: submit the carrier's shared image when we have one; the
            // test pattern stays as the pre-carrier / eye_share=off fallback
            // (pulsing in the headset = carrier pipeline not talking).
            seyes::LatestImage img;
            if (sub::ready() && seyes::latest(e, img)) {
                const State::PoseRec* pr = nullptr;
                if (img.poseId != 0) {
                    const State::PoseRec& r = s.hist[img.poseId % State::kHist];
                    if (r.id == img.poseId) pr = &r;
                }
                if (pr) s.poseHit++; else if (img.poseId) s.poseMiss++; else s.poseNone++;
                sub::draw(s.d3d.ctx, img.srv, ey.rtvs[idx], img.w, img.h, ey.w,
                          ey.h, pr != nullptr);
                if (pr) {
                    // Layer = the rendered pose/FOV (both eyes share one pose id).
                    lpose[e] = pr->pose[e];
                    lfov[e] = pr->fov[e];
                }
                if (img.frameId != s.subLast[e]) {
                    s.subLast[e] = img.frameId;
                    s.subFresh++;
                } else {
                    s.subReused++;  // game ~30 Hz < HMD cadence: re-submit newest
                }
            } else {
                eyes::draw_pattern(s.d3d.ctx, ey.rtvs[idx], e, n);
                s.subPattern++;
            }
            XR_TRY(xrReleaseSwapchainImage(ey.swapchain, nullptr));

            pv[e].pose = lpose[e];
            pv[e].fov = lfov[e];
            pv[e].subImage.swapchain = ey.swapchain;
            pv[e].subImage.imageRect = {{0, 0}, {(int32_t)ey.w, (int32_t)ey.h}};
        }
        // pv[] pose/fov: the carrier-rendered pose when the frame carried a
        // poseId found in the history (S4-4), else the runtime's current views
        // (static-pan frames: nominally head-locked, as in S4-3).
        layer.space = s.space;
        layer.viewCount = 2;
        layer.views = pv;
        layerCount = 1;

        // ~10s submission window (diagnostic; S4-3 acceptance evidence).
        const ULONGLONG now = GetTickCount64();
        if (s.subNext == 0) {
            s.subNext = now + 10000;
        } else if (now > s.subNext) {
            s.subNext = now + 10000;
            // fresh/reused/pattern count per-EYE blits (2 per host frame).
            hostlog::write("submit: window fresh=%llu reused=%llu pattern=%llu "
                           "(eye blits ~%.0f/s, host frames ~%.0f Hz)",
                           (unsigned long long)s.subFresh,
                           (unsigned long long)s.subReused,
                           (unsigned long long)s.subPattern,
                           (s.subFresh + s.subReused + s.subPattern) / 10.0,
                           (s.subFresh + s.subReused + s.subPattern) / 20.0);
            hostlog::write("submit: pose ids hit=%llu miss=%llu none=%llu (miss = "
                           "id aged out of the %u-entry history)",
                           (unsigned long long)s.poseHit, (unsigned long long)s.poseMiss,
                           (unsigned long long)s.poseNone, State::kHist);
            s.poseHit = s.poseMiss = s.poseNone = 0;
            s.subFresh = s.subReused = s.subPattern = 0;
        }
    }

    XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = layerCount;
    ei.layers = layerCount ? layers : nullptr;
    XR_TRY(xrEndFrame(s.session, &ei));
    publish_frame(s, fs, views, poseOk);
    return true;
}

// Publish the located views (or a tracked=false state-only frame) to the
// carrier. Eye fov mirrors XrFovF exactly (tangent half-angles).
void publish_frame(State& s, const XrFrameState& fs, const XrView views[2],
                   bool poseOk) {
    HmdFrame f;
    f.tracked = poseOk;
    f.displayTime = fs.predictedDisplayTime;
    for (int e = 0; e < 2; ++e) {
        f.eye[e].pos = {views[e].pose.position.x, views[e].pose.position.y,
                        views[e].pose.position.z};
        f.eye[e].rot = {views[e].pose.orientation.x, views[e].pose.orientation.y,
                        views[e].pose.orientation.z, views[e].pose.orientation.w};
        f.eye[e].fov = {views[e].fov.angleLeft, views[e].fov.angleRight,
                        views[e].fov.angleUp, views[e].fov.angleDown};
    }
    const float ipd = eye_separation(views);
    State::PoseRec& r = s.hist[(s.pubFrame + 1) % State::kHist];
    r.id = s.pubFrame + 1;  // == the poseId the carrier derives from hostFrame
    for (int e = 0; e < 2; ++e) {
        r.pose[e] = views[e].pose;
        r.fov[e] = views[e].fov;
    }
    ipc::publish(f, ipd, (uint32_t)s.sessionState, s.recenterCount, s.pubFrame++);
}

}  // namespace

int run(const Options& opt) {
    State s;
    if (!create_instance(s) || !create_session(s) || !create_swapchains(s)) {
        hostlog::write("openxr: setup failed");
        return 1;
    }
    hostlog::write("mc2vr_host: ready (openxr session up)");

    // S4-3: blit shaders for the shared-eye submission (non-fatal — on
    // failure the loop keeps submitting the S4-0 test pattern, which is
    // itself the diagnostic).
    sub::init(s.d3d.dev);

    // S4-2: shared-eye mirror (diagnostic window; independent of OpenXR —
    // its failure changes nothing).
    seyes::init(&s.d3d);

    unsigned n = 0;
    bool ok = true;
    while (!s.exiting && ok) {
        handle_events(s);

        // Carrier lifecycle: Shutdown command, or the carrier (game) process
        // went away. Never block — both checks are lock-free polls.
        Mc2IpcMsg cmd;
        while (ipc::pop_command(&cmd)) {
            if (cmd.type == MC2VR_CMD_SHUTDOWN) {
                hostlog::write("openxr: Shutdown command from carrier (pid %u)",
                               ipc::carrier_pid());
                s.selfExit = true;
                if (s.running) xrRequestExitSession(s.session);
                s.exiting = true;
            } else if (cmd.type == MC2VR_CMD_FRAME_READY) {
                // S4-2: {x=frameId y=handle a=slot b=eye c=w d=h}
                seyes::on_frame_ready(cmd.x, cmd.y, cmd.a, cmd.b, cmd.c, cmd.d,
                                     cmd.e);
            } else if (cmd.type == MC2VR_CMD_CONFIG) {
                seyes::on_config(cmd.a, cmd.b, cmd.c);
            } else {
                hostlog::write("openxr: unexpected command %u ignored", cmd.type);
            }
        }
        seyes::pump();
        if (ipc::carrier_died(1000)) {
            s.selfExit = true; // the game is already gone — not a runtime request
            if (s.running) xrRequestExitSession(s.session);
            s.exiting = true;
        }

        if (s.running) {
            ok = frame(s, n++);
            if (opt.maxFrames && (int)n >= opt.maxFrames) {
                s.selfExit = true; // our own frame limit — not a runtime request
                xrRequestExitSession(s.session);
                hostlog::write("openxr: frame limit reached, requesting exit");
                // Keep pumping until the runtime moves us to EXITING.
                for (int i = 0; i < 600 && !s.exiting; ++i) {
                    handle_events(s);
                    if (s.running) frame(s, n++);
                    else Sleep(10);
                }
                break;
            }
        } else {
            Sleep(10);
            // Keep the carrier informed of the session state even while
            // idle (no display time exists yet; state-only publish).
            HmdFrame idle;
            idle.displayTime = 0;
            ipc::publish(idle, 0.0f, (uint32_t)s.sessionState, s.recenterCount,
                         s.pubFrame++);
        }
    }
    hostlog::write("openxr: shutdown (%u frames)", n);
    xrDestroySession(s.session);
    xrDestroyInstance(s.instance);
    return ok ? 0 : 1;
}

}  // namespace xrs
