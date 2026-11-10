// The in-process OpenXR session — see xr_session.hpp.

#include "vr/xr_session.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "game/addresses.h"
#include "log.hpp"
#include "vr/dxvk_interop.hpp"
#include "vr/eye_share.hpp"
#include "vr/render_size.hpp"
#include "vr/xr_api.hpp"
#include "vr/xr_present.hpp"
#include "vr/xr_runtime.hpp"

namespace mc2vr::xr_session {

namespace {

// ---- the runtime's entry points ----------------------------------------------
#define MC2VR_XR_FUNCS(X)                                                                                    \
    X(xrResultToString) X(xrEnumerateInstanceExtensionProperties) X(xrCreateInstance) X(xrDestroyInstance) \
    X(xrGetInstanceProperties) X(xrGetSystem) X(xrGetSystemProperties) X(xrPollEvent)                      \
    X(xrGetVulkanGraphicsRequirementsKHR) X(xrGetVulkanGraphicsDeviceKHR) X(xrGetVulkanInstanceExtensionsKHR) \
    X(xrGetVulkanDeviceExtensionsKHR) X(xrCreateSession)                                                     \
    X(xrDestroySession) X(xrBeginSession) X(xrEndSession) X(xrCreateReferenceSpace) X(xrDestroySpace)      \
    X(xrEnumerateViewConfigurationViews) X(xrEnumerateSwapchainFormats) X(xrCreateSwapchain)               \
    X(xrDestroySwapchain) X(xrEnumerateSwapchainImages) X(xrAcquireSwapchainImage)                         \
    X(xrWaitSwapchainImage) X(xrReleaseSwapchainImage) X(xrWaitFrame) X(xrBeginFrame) X(xrEndFrame)        \
    X(xrLocateViews)

struct Xr {
#define X(name) PFN_##name name = nullptr;
    MC2VR_XR_FUNCS(X)
#undef X
};
Xr xr;
PFN_xrGetInstanceProcAddr g_gipa = nullptr;

bool resolve(XrInstance inst)
{
    bool ok = true;
#define X(name)                                                                                     \
    if (XR_FAILED(g_gipa(inst, #name, reinterpret_cast<PFN_xrVoidFunction *>(&xr.name))) ||         \
        xr.name == nullptr) {                                                                       \
        MC2VR_LOG("xr: runtime lacks %s", #name);                                                   \
        ok = false;                                                                                 \
    }
    MC2VR_XR_FUNCS(X)
#undef X
    return ok;
}

// ---- published state (any thread) ----------------------------------------------
SRWLOCK g_state_lock = SRWLOCK_INIT;
CONDITION_VARIABLE g_views_cv = CONDITION_VARIABLE_INIT;
ViewState g_state;            // under g_state_lock
bool g_active = false;        // a session exists (under g_state_lock)
std::atomic<bool> g_presenting{false};
std::atomic<uint32_t> g_view_size{0};  // recommended per-eye size, w << 16 | h (0 = none / applied)

bool g_started = false;
IDirect3DDevice9 *g_d3d = nullptr;

// ---- session thread state ---------------------------------------------------------
dxvk::ID3D9VkInteropDevice *g_interop = nullptr;
XrInstance g_inst = XR_NULL_HANDLE;
XrSystemId g_system = XR_NULL_SYSTEM_ID;
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_space = XR_NULL_HANDLE;
XrSessionState g_xr_state = XR_SESSION_STATE_UNKNOWN;
bool g_running = false;  // between xrBeginSession and xrEndSession
bool g_exiting = false;
uint32_t g_max_w = 0, g_max_h = 0;
std::vector<int64_t> g_formats;

struct Eye {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageVulkanKHR> images;
};
Eye g_eye[2];
uint32_t g_sc_w = 0, g_sc_h = 0;
VkFormat g_sc_src_format = VK_FORMAT_UNDEFINED;  // eye image format the swapchains were made for
bool g_sc_failed = false;                          // creation failed for this size/format (logged once)

// Views published per pose id; the layer is submitted with the views the
// pair was rendered from.
struct PoseRec {
    uint32_t id = 0;
    XrPosef pose[2];
    XrFovf fov[2];
};
constexpr uint32_t kHist = 256;
PoseRec g_hist[kHist];
uint32_t g_pub = 0;  // last published pose id

// The layer last submitted (re-submitted while no new pair arrives).
bool g_have_layer = false;
XrPosef g_layer_pose[2];
XrFovf g_layer_fov[2];

// Lockstep (docs/pacing.md): the wait for the pair rendered from this frame's
// views is bounded, and only made while pairs are arriving.
constexpr uint32_t kPairTimeoutMs = 100;
constexpr ULONGLONG kPairActiveMs = 500;
uint64_t g_last_frame = 0;
ULONGLONG g_last_pair_ms = 0;
bool g_wrong_device_logged = false;

// 10 s window
uint64_t w_frames = 0, w_fresh = 0, w_late = 0, w_stale = 0, w_empty = 0, w_copy_fail = 0, w_end_layer_fail = 0;
uint64_t w_pose_hit = 0, w_pose_miss = 0, w_pose_none = 0, w_wait_n = 0;
double w_wait_sum = 0.0, w_wait_max = 0.0;
ULONGLONG w_next = 0;

double qpc_ms()
{
    static LARGE_INTEGER f = {};
    if (f.QuadPart == 0) {
        QueryPerformanceFrequency(&f);
    }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

const char *result_name(XrResult r)
{
    static char buf[XR_MAX_RESULT_STRING_SIZE];
    if (g_inst == XR_NULL_HANDLE || xr.xrResultToString == nullptr || XR_FAILED(xr.xrResultToString(g_inst, r, buf))) {
        snprintf(buf, sizeof(buf), "XrResult %d", (int)r);
    }
    return buf;
}

// Logs a failed call (first few of each site only for per-frame calls).
bool ok(XrResult r, const char *what)
{
    if (XR_SUCCEEDED(r)) {
        return true;
    }
    static uint32_t logged = 0;
    if (logged++ < 32) {
        MC2VR_LOG("xr: %s failed: %s", what, result_name(r));
    }
    return false;
}

const char *state_name(XrSessionState s)
{
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

EyePose to_eye(const XrView &v)
{
    EyePose e;
    e.pos = {v.pose.position.x, v.pose.position.y, v.pose.position.z};
    e.rot = {v.pose.orientation.x, v.pose.orientation.y, v.pose.orientation.z, v.pose.orientation.w};
    e.fov = {v.fov.angleLeft, v.fov.angleRight, v.fov.angleUp, v.fov.angleDown};
    return e;
}

// Publishes a frame's views (views == nullptr: session state only) and
// releases the render gate. Returns the new pose id.
uint32_t publish(const XrFrameState *fs, const XrView *views, bool tracked)
{
    AcquireSRWLockExclusive(&g_state_lock);
    g_state.session_state = (uint32_t)g_xr_state;
    if (fs != nullptr) {
        const uint32_t id = ++g_pub;
        g_state.pose_id = id;
        g_state.tracked = views != nullptr && tracked;
        g_state.display_time = fs->predictedDisplayTime;
        g_state.display_period = fs->predictedDisplayPeriod;
        if (views != nullptr) {
            g_state.eye[0] = to_eye(views[0]);
            g_state.eye[1] = to_eye(views[1]);
            PoseRec &r = g_hist[id % kHist];
            r.id = id;
            for (int e = 0; e < 2; e++) {
                r.pose[e] = views[e].pose;
                r.fov[e] = views[e].fov;
            }
        }
    } else {
        g_state.tracked = false;
    }
    const uint32_t id = g_pub;
    ReleaseSRWLockExclusive(&g_state_lock);
    WakeAllConditionVariable(&g_views_cv);
    return id;
}

// The runtime ends the session (SteamVR quitting, session lost): quit the game
// cleanly. The engine's own quit path is VM-protected, so the window message
// is the signal (addresses.h MC2_G_RENDER_HWND).
void request_game_exit(const char *why)
{
    static bool sent = false;
    if (sent) return;
    sent = true;
    HWND hwnd = (HWND)*(volatile uintptr_t *)MC2_G_RENDER_HWND;
    if (hwnd != nullptr) {
        if (HWND root = GetAncestor(hwnd, GA_ROOT)) hwnd = root;
    }
    if (hwnd != nullptr && PostMessageW(hwnd, WM_CLOSE, 0, 0) != 0) {
        MC2VR_LOG("xr: %s — posted WM_CLOSE to the game window %p", why, (void *)hwnd);
    } else {
        MC2VR_LOG("xr: %s — no game window to close (hwnd=%p)", why, (void *)hwnd);
    }
}

// ---- setup ----------------------------------------------------------------------------

bool setup()
{
    if (FAILED(g_d3d->QueryInterface(dxvk::kIID_InteropDevice, reinterpret_cast<void **>(&g_interop))) ||
        g_interop == nullptr) {
        MC2VR_LOG("xr: the D3D9 device has no ID3D9VkInteropDevice — mc2vr needs DXVK's d3d9.dll "
                  "(docs/platforms.md)");
        return false;
    }
    VkInstance vk_inst = VK_NULL_HANDLE;
    VkPhysicalDevice vk_phys = VK_NULL_HANDLE;
    VkDevice vk_dev = VK_NULL_HANDLE;
    VkQueue vk_queue = VK_NULL_HANDLE;
    uint32_t q_index = 0, q_family = 0;
    g_interop->GetVulkanHandles(&vk_inst, &vk_phys, &vk_dev);
    g_interop->GetSubmissionQueue(&vk_queue, &q_index, &q_family);
    MC2VR_LOG("xr: DXVK Vulkan: instance %p physical %p device %p queue %p (family %u index %u)",
              (void *)vk_inst, (void *)vk_phys, (void *)vk_dev, (void *)vk_queue, q_family, q_index);

    g_gipa = xr_runtime::load();
    if (g_gipa == nullptr) {
        return false;
    }
    if (XR_FAILED(g_gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
                         reinterpret_cast<PFN_xrVoidFunction *>(&xr.xrEnumerateInstanceExtensionProperties))) ||
        XR_FAILED(g_gipa(XR_NULL_HANDLE, "xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction *>(&xr.xrCreateInstance)))) {
        MC2VR_LOG("xr: runtime lacks the global functions");
        return false;
    }
    uint32_t n = 0;
    bool has_vk = false;
    if (XR_SUCCEEDED(xr.xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr)) && n > 0) {
        std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
        xr.xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data());
        for (uint32_t i = 0; i < n; i++) {
            has_vk = has_vk || strcmp(props[i].extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0;
        }
    }
    if (!has_vk) {
        MC2VR_LOG("xr: runtime lacks " XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
        return false;
    }

    const char *exts[] = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ici.applicationInfo.applicationName, "Mercenaries 2 (mc2vr)");
    ici.applicationInfo.applicationVersion = 1;
    strcpy(ici.applicationInfo.engineName, "mc2vr");
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = exts;
    if (!ok(xr.xrCreateInstance(&ici, &g_inst), "xrCreateInstance")) {
        return false;
    }
    if (!resolve(g_inst)) {
        return false;
    }
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xr.xrGetInstanceProperties(g_inst, &ip))) {
        MC2VR_LOG("xr: runtime '%s' %u.%u.%u", ip.runtimeName, (unsigned)XR_VERSION_MAJOR(ip.runtimeVersion),
                  (unsigned)XR_VERSION_MINOR(ip.runtimeVersion), (unsigned)XR_VERSION_PATCH(ip.runtimeVersion));
    }

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!ok(xr.xrGetSystem(g_inst, &sgi, &g_system), "xrGetSystem(HMD) (headset connected? SteamVR up?)")) {
        return false;
    }
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    if (ok(xr.xrGetSystemProperties(g_inst, g_system, &sp), "xrGetSystemProperties")) {
        g_max_w = sp.graphicsProperties.maxSwapchainImageWidth;
        g_max_h = sp.graphicsProperties.maxSwapchainImageHeight;
        MC2VR_LOG("xr: system '%s' maxSwapchain %ux%u", sp.systemName, g_max_w, g_max_h);
    }

    // Required before xrCreateSession (XR_KHR_vulkan_enable).
    XrGraphicsRequirementsVulkanKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    if (!ok(xr.xrGetVulkanGraphicsRequirementsKHR(g_inst, g_system, &req), "xrGetVulkanGraphicsRequirementsKHR")) {
        return false;
    }
    // What the runtime needs DXVK's instance and device to have enabled.
    // Under Proton DXVK enables these itself (the Wine VR registry,
    // docs/openxr_proton.md item 2); logged so a session failure is explainable.
    {
        static char list[4096];
        uint32_t len = 0;
        if (XR_SUCCEEDED(xr.xrGetVulkanInstanceExtensionsKHR(g_inst, g_system, sizeof(list), &len, list))) {
            MC2VR_LOG("xr: runtime needs Vulkan instance extensions: %s", list);
        }
        if (XR_SUCCEEDED(xr.xrGetVulkanDeviceExtensionsKHR(g_inst, g_system, sizeof(list), &len, list))) {
            MC2VR_LOG("xr: runtime needs Vulkan device extensions: %s", list);
        }
    }
    VkPhysicalDevice want = VK_NULL_HANDLE;
    if (ok(xr.xrGetVulkanGraphicsDeviceKHR(g_inst, g_system, vk_inst, &want), "xrGetVulkanGraphicsDeviceKHR") &&
        want != vk_phys) {
        MC2VR_LOG("xr: WARNING the runtime wants physical device %p, DXVK uses %p", (void *)want, (void *)vk_phys);
    }

    XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    binding.instance = vk_inst;
    binding.physicalDevice = vk_phys;
    binding.device = vk_dev;
    binding.queueFamilyIndex = q_family;
    binding.queueIndex = q_index;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = g_system;
    XrResult r;
    {
        dxvk::QueueLock lock(g_interop);
        r = xr.xrCreateSession(g_inst, &sci, &g_session);
    }
    if (!ok(r, "xrCreateSession (DXVK device + queue)")) {
        return false;
    }
    XrReferenceSpaceCreateInfo rsi{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsi.poseInReferenceSpace.orientation.w = 1.0f;
    if (!ok(xr.xrCreateReferenceSpace(g_session, &rsi, &g_space), "xrCreateReferenceSpace(LOCAL)")) {
        return false;
    }

    XrViewConfigurationView vcv[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    uint32_t nv = 0;
    if (!ok(xr.xrEnumerateViewConfigurationViews(g_inst, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
                                                 &nv, vcv),
            "xrEnumerateViewConfigurationViews") ||
        nv != 2) {
        return false;
    }
    MC2VR_LOG("xr: recommended per-eye size %ux%u", vcv[0].recommendedImageRectWidth,
              vcv[0].recommendedImageRectHeight);
    g_view_size.store((vcv[0].recommendedImageRectWidth << 16) | (vcv[0].recommendedImageRectHeight & 0xffff));

    uint32_t nf = 0;
    if (ok(xr.xrEnumerateSwapchainFormats(g_session, 0, &nf, nullptr), "xrEnumerateSwapchainFormats") && nf > 0) {
        g_formats.resize(nf);
        xr.xrEnumerateSwapchainFormats(g_session, nf, &nf, g_formats.data());
        char list[512] = "";
        for (uint32_t i = 0; i < nf; i++) {
            const size_t len = strlen(list);
            snprintf(list + len, sizeof(list) - len, "%s%lld", i ? " " : "", (long long)g_formats[i]);
        }
        MC2VR_LOG("xr: swapchain formats (VkFormat): %s", list);
    }

    if (!xr_present::init(g_interop, vk_inst, vk_dev, vk_queue, q_family)) {
        return false;
    }

    AcquireSRWLockExclusive(&g_state_lock);
    g_active = true;
    ReleaseSRWLockExclusive(&g_state_lock);
    MC2VR_LOG("xr: session created on DXVK's device");
    return true;
}

// ---- swapchains ---------------------------------------------------------------------

void destroy_swapchains()
{
    xr_present::wait_idle();
    dxvk::QueueLock lock(g_interop);
    for (Eye &e : g_eye) {
        if (e.swapchain != XR_NULL_HANDLE) xr.xrDestroySwapchain(e.swapchain);
        e.swapchain = XR_NULL_HANDLE;
        e.images.clear();
    }
    g_have_layer = false;
}

// The swapchain format for eye images of `src`: its sRGB sibling, so the raw
// copy hands the compositor sRGB-encoded bytes it decodes (docs/xr_session.md).
int64_t pick_format(VkFormat src, bool *exact)
{
    const int64_t srgb = src == VK_FORMAT_B8G8R8A8_UNORM   ? VK_FORMAT_B8G8R8A8_SRGB
                         : src == VK_FORMAT_R8G8B8A8_UNORM ? VK_FORMAT_R8G8B8A8_SRGB
                                                           : 0;
    for (int64_t f : g_formats) {
        if (srgb != 0 && f == srgb) {
            *exact = true;
            return f;
        }
    }
    for (int64_t f : g_formats) {
        if (f == src) {
            *exact = false;
            return f;
        }
    }
    return 0;
}

bool create_swapchains(uint32_t w, uint32_t h, int64_t fmt);

// Swapchains at the eye images' own size and format. False when they can't be.
bool ensure_swapchains(uint32_t w, uint32_t h, VkFormat src)
{
    if (w == g_sc_w && h == g_sc_h && src == g_sc_src_format) {
        return !g_sc_failed;  // made (both complete) or already failed for this size/format
    }
    destroy_swapchains();
    g_sc_w = w;
    g_sc_h = h;
    g_sc_src_format = src;
    g_sc_failed = true;
    if ((g_max_w && w > g_max_w) || (g_max_h && h > g_max_h)) {
        MC2VR_LOG("xr: eye images %ux%u exceed the runtime's max swapchain %ux%u — not shown", w, h, g_max_w,
                  g_max_h);
        return false;
    }
    bool exact = false;
    const int64_t fmt = pick_format(src, &exact);
    if (fmt == 0) {
        MC2VR_LOG("xr: no swapchain format byte-compatible with the eye images (VkFormat %d) — not shown",
                  (int)src);
        return false;
    }
    if (!exact) {
        MC2VR_LOG("xr: WARNING no sRGB swapchain format for VkFormat %d; using it as-is (gamma will be off)",
                  (int)src);
    }
    if (!create_swapchains(w, h, fmt)) {
        destroy_swapchains();  // never leave a half-made pair
        return false;
    }
    g_sc_failed = false;
    MC2VR_LOG("xr: swapchains %ux%u format %lld, %u/%u images", w, h, (long long)fmt,
              (unsigned)g_eye[0].images.size(), (unsigned)g_eye[1].images.size());
    return true;
}

// Acquire with a successful wait (the only state an image may be released
// from). Waits retry on XR_TIMEOUT_EXPIRED, as the spec requires.
bool acquire_and_wait(Eye &ey, uint32_t *idx, bool *acquired)
{
    {
        dxvk::QueueLock lock(g_interop);
        *acquired = ok(xr.xrAcquireSwapchainImage(ey.swapchain, nullptr, idx), "xrAcquireSwapchainImage");
    }
    if (!*acquired || *idx >= ey.images.size()) {
        return false;
    }
    for (int attempt = 0; attempt < 50; attempt++) {
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = 100ll * 1000 * 1000;
        const XrResult r = xr.xrWaitSwapchainImage(ey.swapchain, &wi);
        if (r == XR_TIMEOUT_EXPIRED) {
            continue;
        }
        return ok(r, "xrWaitSwapchainImage");
    }
    MC2VR_LOG("xr: xrWaitSwapchainImage still timing out after 5 s");
    return false;
}

// Acquires an image of each eye's swapchain, copies the pair into them and
// releases them. False when nothing usable was produced.
bool present_pair(const eye_share::Pair &p)
{
    uint32_t idx[2] = {};
    bool acquired[2] = {}, waited[2] = {};
    waited[0] = acquire_and_wait(g_eye[0], &idx[0], &acquired[0]);
    waited[1] = waited[0] && acquire_and_wait(g_eye[1], &idx[1], &acquired[1]);
    bool good = waited[0] && waited[1];
    if (good) {
        const VkImage dst[2] = {g_eye[0].images[idx[0]].image, g_eye[1].images[idx[1]].image};
        good = xr_present::copy_pair(p.eye, dst, p.w, p.h);
    }
    // Released either way (a failed copy leaves the image's old contents).
    // An acquired image whose wait failed can't be released; the swapchain
    // is re-created instead.
    bool stuck = false;
    {
        dxvk::QueueLock lock(g_interop);
        for (int e = 0; e < 2; e++) {
            if (waited[e]) {
                ok(xr.xrReleaseSwapchainImage(g_eye[e].swapchain, nullptr), "xrReleaseSwapchainImage");
            } else if (acquired[e]) {
                stuck = true;
            }
        }
    }
    if (stuck) {
        destroy_swapchains();
        g_sc_w = g_sc_h = 0;  // the next pair re-creates them
    }
    return good;
}

// Both eyes' swapchains (caller cleans up on failure).
bool create_swapchains(uint32_t w, uint32_t h, int64_t fmt)
{
    dxvk::QueueLock lock(g_interop);
    for (int e = 0; e < 2; e++) {
        XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        sc.format = fmt;
        sc.sampleCount = 1;
        sc.width = w;
        sc.height = h;
        sc.faceCount = 1;
        sc.arraySize = 1;
        sc.mipCount = 1;
        Eye &ey = g_eye[e];
        uint32_t ni = 0;
        if (!ok(xr.xrCreateSwapchain(g_session, &sc, &ey.swapchain), "xrCreateSwapchain")) {
            ey.swapchain = XR_NULL_HANDLE;
            return false;
        }
        if (
            !ok(xr.xrEnumerateSwapchainImages(ey.swapchain, 0, &ni, nullptr), "xrEnumerateSwapchainImages")) {
            return false;
        }
        ey.images.assign(ni, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        if (!ok(xr.xrEnumerateSwapchainImages(ey.swapchain, ni, &ni,
                                              reinterpret_cast<XrSwapchainImageBaseHeader *>(ey.images.data())),
                "xrEnumerateSwapchainImages") ||
            ni == 0) {
            return false;
        }
    }
    return true;
}

// ---- events ---------------------------------------------------------------------------

void handle_events()
{
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xr.xrPollEvent(g_inst, &ev) == XR_SUCCESS) {
        switch (ev.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            const auto &sc = *reinterpret_cast<const XrEventDataSessionStateChanged *>(&ev);
            g_xr_state = sc.state;
            MC2VR_LOG("xr: session state -> %s", state_name(sc.state));
            if (sc.state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                dxvk::QueueLock lock(g_interop);
                g_running = ok(xr.xrBeginSession(g_session, &bi), "xrBeginSession");
            } else if (sc.state == XR_SESSION_STATE_STOPPING) {
                dxvk::QueueLock lock(g_interop);
                ok(xr.xrEndSession(g_session), "xrEndSession");
                g_running = false;
            } else if (sc.state == XR_SESSION_STATE_EXITING || sc.state == XR_SESSION_STATE_LOSS_PENDING) {
                request_game_exit("the runtime ended the session");
                g_exiting = true;
            }
            g_presenting = g_running && lockstep((uint32_t)sc.state);
            publish(nullptr, nullptr, false);
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            request_game_exit("the runtime is going away (instance loss pending)");
            g_exiting = true;
            break;
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
            // Nothing to apply: the camera follows the published views, so a
            // recenter shows at the next pass-1 sample.
            MC2VR_LOG("xr: reference space change pending (recenter)");
            break;
        default:
            break;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// ---- the frame loop -------------------------------------------------------------------

void report(ULONGLONG now, const XrView *views)
{
    if (w_next == 0) {
        w_next = now + 10000;
        return;
    }
    if (now < w_next) {
        return;
    }
    w_next = now + 10000;
    MC2VR_LOG("submit: frames=%llu (~%.0f Hz) fresh=%llu late=%llu stale=%llu empty=%llu copyFail=%llu "
              "layerRejected=%llu | "
              "pair wait avg=%.2fms max=%.2fms | pose ids hit=%llu miss=%llu none=%llu",
              (unsigned long long)w_frames, w_frames / 10.0, (unsigned long long)w_fresh,
              (unsigned long long)w_late, (unsigned long long)w_stale, (unsigned long long)w_empty,
              (unsigned long long)w_copy_fail, (unsigned long long)w_end_layer_fail, w_wait_n ? w_wait_sum / (double)w_wait_n : 0.0, w_wait_max,
              (unsigned long long)w_pose_hit, (unsigned long long)w_pose_miss, (unsigned long long)w_pose_none);
    if (views != nullptr) {
        MC2VR_LOG("submit: view L pos (%.3f %.3f %.3f) fov (%.3f %.3f %.3f %.3f)", views[0].pose.position.x,
                  views[0].pose.position.y, views[0].pose.position.z, views[0].fov.angleLeft,
                  views[0].fov.angleRight, views[0].fov.angleUp, views[0].fov.angleDown);
    }
    w_frames = w_fresh = w_late = w_stale = w_empty = w_copy_fail = w_end_layer_fail = 0;
    w_pose_hit = w_pose_miss = w_pose_none = w_wait_n = 0;
    w_wait_sum = w_wait_max = 0.0;
}

bool frame()
{
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    if (!ok(xr.xrWaitFrame(g_session, nullptr, &fs), "xrWaitFrame")) {
        return false;
    }
    {
        dxvk::QueueLock lock(g_interop);
        if (!ok(xr.xrBeginFrame(g_session, nullptr), "xrBeginFrame")) {
            return false;
        }
    }
    w_frames++;
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    bool views_ok = false;
    XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                              {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const XrCompositionLayerBaseHeader *layers[1] = {reinterpret_cast<XrCompositionLayerBaseHeader *>(&layer)};
    uint32_t layer_count = 0;

    if (fs.shouldRender) {
        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
        li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        li.displayTime = fs.predictedDisplayTime;
        li.space = g_space;
        XrViewState vs{XR_TYPE_VIEW_STATE};
        uint32_t nv = 0;
        if (ok(xr.xrLocateViews(g_session, &li, &vs, 2, &nv, views), "xrLocateViews") && nv == 2) {
            views_ok = (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
                       (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
        }
        // This frame's views = the render pose: publishing releases the gate.
        const uint32_t want = publish(&fs, views, views_ok);

        const ULONGLONG now = GetTickCount64();
        const uint32_t timeout = now - g_last_pair_ms < kPairActiveMs ? kPairTimeoutMs : 0;
        const double t0 = qpc_ms();
        eye_share::Pair p;
        const eye_share::Take take = eye_share::take_pair(g_last_frame, want, timeout, &p);
        if (timeout != 0) {
            const double w = qpc_ms() - t0;
            w_wait_sum += w;
            w_wait_max = w > w_wait_max ? w : w_wait_max;
            w_wait_n++;
        }
        if (take != eye_share::Take::None) {
            g_last_frame = p.frame_id;
            g_last_pair_ms = now;
            bool shown = false;
            if (p.device != g_d3d) {
                if (!g_wrong_device_logged) {
                    g_wrong_device_logged = true;
                    MC2VR_LOG("xr: the game re-created its D3D9 device — the session is bound to the old one; "
                              "nothing more is shown");
                }
            } else if (ensure_swapchains(p.w, p.h, p.format)) {
                shown = present_pair(p);
                if (!shown) {
                    w_copy_fail++;
                    g_have_layer = false;  // the released images hold no finished pair
                }
            }
            eye_share::release_pair(p);  // the copy is submitted: DXVK's later writes queue after it
            if (shown) {
                // The views the pair was rendered from; else this frame's
                // (valid ones only — a zero quaternion fails xrEndFrame); else
                // the previous layer's.
                const PoseRec &r = g_hist[p.pose_id % kHist];
                const bool hit = p.pose_id != 0 && r.id == p.pose_id;
                if (hit || views_ok) {
                    for (int e = 0; e < 2; e++) {
                        g_layer_pose[e] = hit ? r.pose[e] : views[e].pose;
                        g_layer_fov[e] = hit ? r.fov[e] : views[e].fov;
                    }
                    g_have_layer = true;
                }
                (hit ? w_pose_hit : p.pose_id ? w_pose_miss : w_pose_none)++;
                (take == eye_share::Take::Fresh ? w_fresh : w_late)++;
            }
        } else if (g_have_layer) {
            w_stale++;  // the last pair again (its swapchain images still hold it)
        } else {
            w_empty++;
        }
        if (g_have_layer) {
            for (int e = 0; e < 2; e++) {
                pv[e].pose = g_layer_pose[e];
                pv[e].fov = g_layer_fov[e];
                pv[e].subImage.swapchain = g_eye[e].swapchain;
                pv[e].subImage.imageRect = {{0, 0}, {(int32_t)g_sc_w, (int32_t)g_sc_h}};
            }
            layer.space = g_space;
            layer.viewCount = 2;
            layer.views = pv;
            layer_count = 1;
        }
        report(now, views_ok ? views : nullptr);
    } else {
        publish(&fs, nullptr, false);
    }

    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = layer_count;
    ei.layers = layer_count ? layers : nullptr;
    XrResult r;
    {
        dxvk::QueueLock lock(g_interop);
        r = xr.xrEndFrame(g_session, &ei);
    }
    if (XR_FAILED(r) && layer_count != 0) {
        // A rejected layer: drop it (re-submitting it would fail every
        // frame) and keep the session; the next pair makes a new one.
        ok(r, "xrEndFrame (layer dropped)");
        g_have_layer = false;
        w_end_layer_fail++;
        return true;
    }
    return ok(r, "xrEndFrame");
}

void teardown()
{
    g_presenting = false;
    AcquireSRWLockExclusive(&g_state_lock);
    g_active = false;
    ReleaseSRWLockExclusive(&g_state_lock);
    WakeAllConditionVariable(&g_views_cv);
    if (g_session != XR_NULL_HANDLE && g_interop != nullptr) {
        if (xr.xrDestroySwapchain) destroy_swapchains();
        if (g_space != XR_NULL_HANDLE && xr.xrDestroySpace) xr.xrDestroySpace(g_space);
        dxvk::QueueLock lock(g_interop);
        if (g_running && xr.xrEndSession) xr.xrEndSession(g_session);
        if (xr.xrDestroySession) xr.xrDestroySession(g_session);
    }
    if (g_inst != XR_NULL_HANDLE && xr.xrDestroyInstance) xr.xrDestroyInstance(g_inst);
    g_session = XR_NULL_HANDLE;
    g_inst = XR_NULL_HANDLE;
}

DWORD WINAPI session_thread(void *)
{
    if (!setup()) {
        MC2VR_LOG("xr: no OpenXR session — VR off for this run (camera pass-through)");
        teardown();
        return 0;
    }
    uint32_t frame_errors = 0;
    while (!g_exiting) {
        handle_events();
        if (g_exiting) break;
        if (!g_running) {
            Sleep(10);
            continue;
        }
        if (frame()) {
            frame_errors = 0;
        } else if (++frame_errors > 100) {
            MC2VR_LOG("xr: frame loop keeps failing — ending the session");
            break;
        } else {
            Sleep(1);
        }
    }
    teardown();
    MC2VR_LOG("xr: session thread done");
    return 0;
}

} // namespace

void start(IDirect3DDevice9 *device)
{
    if (g_started || device == nullptr) {
        return;
    }
    g_started = true;
    g_d3d = device;
    HANDLE t = CreateThread(nullptr, 0, session_thread, nullptr, 0, nullptr);
    if (t == nullptr) {
        MC2VR_LOG("xr: CreateThread failed (%lu) — VR off", GetLastError());
        return;
    }
    CloseHandle(t);  // leaked by design, like every carrier thread
}

bool read_state(ViewState *out)
{
    AcquireSRWLockShared(&g_state_lock);
    const bool active = g_active;
    if (active) *out = g_state;
    ReleaseSRWLockShared(&g_state_lock);
    return active;
}

bool wait_views(uint32_t last_pose_id, uint32_t timeout_ms, ViewState *out, bool *timed_out)
{
    *timed_out = false;
    const double t0 = qpc_ms();
    AcquireSRWLockShared(&g_state_lock);
    while (g_active && lockstep(g_state.session_state) && g_state.pose_id == last_pose_id) {
        const double left = (double)timeout_ms - (qpc_ms() - t0);
        if (left <= 0.0) {
            *timed_out = true;
            break;
        }
        SleepConditionVariableSRW(&g_views_cv, &g_state_lock, (DWORD)left + 1, CONDITION_VARIABLE_LOCKMODE_SHARED);
    }
    const bool active = g_active;
    if (active) *out = g_state;
    ReleaseSRWLockShared(&g_state_lock);
    return active;
}

bool presenting()
{
    return g_presenting.load(std::memory_order_relaxed);
}

void on_update_tick()
{
    const uint32_t size = g_view_size.exchange(0);
    if (size != 0) {
        render_size::on_hmd_view_size(size >> 16, size & 0xffff);
    }
}

void wait_gpu_copies()
{
    xr_present::wait_idle();
}

} // namespace mc2vr::xr_session
