// The OpenXR session, woven into the stereo frame — see xr_session.hpp.

#include "vr/xr_session.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "game/addresses.h"
#include "log.hpp"
#include "qpc.hpp"
#include "vr/dxvk_interop.hpp"
#include "vr/eye_share.hpp"
#include "vr/fpu_precision.hpp"
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
PFN_xrConvertWin32PerformanceCounterToTimeKHR g_qpc_to_xr = nullptr;  // optional

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
// The latest frame's views, for the update thread (camera table, render size).
SRWLOCK g_state_lock = SRWLOCK_INIT;
ViewState g_state;      // under g_state_lock
bool g_active = false;  // a session exists (under g_state_lock)
std::atomic<uint32_t> g_view_size{0};  // recommended per-eye size, w << 16 | h (0 = none / applied)
std::atomic<IDirect3DDevice9 *> g_d3d{nullptr};  // start(): the game's device

// ---- session objects (created on the render thread at setup) ------------------
enum class Setup { NotYet, Ready, Off };
Setup g_setup = Setup::NotYet;  // render thread
dxvk::ID3D9VkInteropDevice *g_interop = nullptr;
XrInstance g_inst = XR_NULL_HANDLE;
XrSystemId g_system = XR_NULL_SYSTEM_ID;
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_space = XR_NULL_HANDLE;
uint32_t g_max_w = 0, g_max_h = 0;
std::vector<int64_t> g_formats;
HANDLE g_frame_thread = nullptr;

// ---- frame thread -----------------------------------------------------------------
XrSessionState g_xr_state = XR_SESSION_STATE_UNKNOWN;
std::atomic<bool> g_running{false};  // between xrBeginSession and xrEndSession (frame thread writes)
bool g_exiting = false;
bool g_stop_pending = false;  // STOPPING arrived while a frame was open: end the session after it
uint32_t g_frame_errors = 0;  // consecutive frame-call failures

// ---- the hand-off between the two threads (under g_hand_lock) -----------------------
// One frame in flight: the frame thread begins it (available), the render
// thread claims it and later hands its layer back (end_ready), the frame thread
// ends it.
CRITICAL_SECTION g_hand_lock;
CONDITION_VARIABLE g_hand_cv = CONDITION_VARIABLE_INIT;
struct Handoff {
    bool available = false;  // a begun frame (frame thread → render thread)
    bool claimed = false;    // the render thread works on it
    bool end_ready = false;  // its layer is handed back (render thread → frame thread)
    bool stopped = false;    // the frame thread has ended (exit, errors): VR off
    bool presenting = false; // the render thread is acquiring/copying/releasing for it: not withdrawn
    uint32_t serial = 0;     // the begun frame's number
    uint32_t end_serial = 0; // which frame end_ready belongs to
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    uint32_t session_state = 0;
    bool has_layer = false;
    XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                              {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
};
Handoff g_hand;
constexpr DWORD kBeginWaitMs = 100;  // render thread: longest wait for a begun frame
constexpr int kEndWaitTicks = 20;    // frame thread: 10 ms ticks to wait for a claimed frame when stopping
bool g_begin_timed_out = false;      // render thread: the last wait ran out; don't wait again until a frame is offered

// ---- render thread ------------------------------------------------------------------
uint32_t g_frame_id = 0;  // views counter (ViewState::pose_id)
uint32_t g_claimed_serial = 0;  // the claimed frame not yet handed back (0 = none)
struct Eye {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageVulkanKHR> images;
};
Eye g_eye[2];
uint32_t g_sc_w = 0, g_sc_h = 0;
VkFormat g_sc_src_format = VK_FORMAT_UNDEFINED;  // eye image format the swapchains were made for
bool g_sc_failed = false;                          // creation failed for this size/format (logged once)
bool g_wrong_device_logged = false;

// 10 s window. Frame counts and the pose latency: render thread (it reports);
// the per-call stats and the end margin: either thread, under g_stat_lock.
uint64_t w_frames = 0, w_shown = 0, w_hidden = 0, w_no_pair = 0, w_no_pose = 0, w_copy_fail = 0,
         w_no_frame = 0;
double w_pose_sum = 0.0, w_pose_max = 0.0;
uint64_t w_pose_n = 0;
uint64_t w_depth[5] = {};  // SteamVR's prediction depth, display periods (4 = 4+)
ULONGLONG w_next = 0;
SRWLOCK g_stat_lock = SRWLOCK_INIT;
double w_margin_sum = 0.0, w_margin_min = 0.0;  // xrEndFrame -> display (frame thread)
uint64_t w_margin_n = 0, w_end_layer_fail = 0;

// Now as XrTime, for the latency log. Two candidates, picked once against a
// predicted display time (the right one lands within a second of it):
//   - XR_KHR_win32_convert_performance_counter_time (Windows runtimes);
//   - the QPC itself in ns: under Wine the QPC counts CLOCK_MONOTONIC since
//     boot, which is SteamVR's XrTime on Linux. wineopenxr's conversion
//     returned ~0 there (2026-11-10), so this is the Proton path.
enum class Clock { Unknown, Extension, Qpc, None };
Clock g_clock = Clock::Unknown;  // calibrated once, under g_clock_lock (both threads use it)
SRWLOCK g_clock_lock = SRWLOCK_INIT;

XrTime qpc_ns()
{
    static LARGE_INTEGER f = {};
    if (f.QuadPart == 0) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (XrTime)((double)t.QuadPart * (1e9 / (double)f.QuadPart));
}

XrTime ext_ns()
{
    if (g_qpc_to_xr == nullptr) return 0;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    XrTime x = 0;
    return XR_SUCCEEDED(g_qpc_to_xr(g_inst, &t, &x)) ? x : 0;
}

// `display` = a predicted display time (calibrates on first use). 0 = no clock.
XrTime xr_now(XrTime display)
{
    AcquireSRWLockShared(&g_clock_lock);
    Clock clock = g_clock;
    ReleaseSRWLockShared(&g_clock_lock);
    if (clock == Clock::Unknown) {
        AcquireSRWLockExclusive(&g_clock_lock);
        if (g_clock != Clock::Unknown) {
            clock = g_clock;
            ReleaseSRWLockExclusive(&g_clock_lock);
            return clock == Clock::Extension ? ext_ns() : clock == Clock::Qpc ? qpc_ns() : 0;
        }
        const XrTime e = ext_ns(), q = qpc_ns();
        auto close_to = [&](XrTime t) { return t != 0 && t > display - 1000000000ll && t < display + 1000000000ll; };
        g_clock = close_to(e) ? Clock::Extension : close_to(q) ? Clock::Qpc : Clock::None;
        MC2VR_LOG("xr: latency clock: %s (display - now: extension %.1f ms, QPC %.1f ms)",
                  g_clock == Clock::Extension ? "the runtime's QPC conversion"
                  : g_clock == Clock::Qpc     ? "QPC as CLOCK_MONOTONIC ns"
                                              : "none fits — latency log off",
                  e ? (double)(display - e) / 1e6 : 0.0, (double)(display - q) / 1e6);
        clock = g_clock;
        ReleaseSRWLockExclusive(&g_clock_lock);
    }
    return clock == Clock::Extension ? ext_ns() : clock == Clock::Qpc ? qpc_ns() : 0;
}

// Per-call timing, 10 s window (submit: calls line).
struct Stat {
    double sum = 0.0, max = 0.0;
    uint64_t n = 0;
    void add(double v)
    {
        AcquireSRWLockExclusive(&g_stat_lock);
        sum += v;
        max = v > max ? v : max;
        n++;
        ReleaseSRWLockExclusive(&g_stat_lock);
    }
    double avg() const { return n ? sum / (double)n : 0.0; }
};
// frameWait = the render thread waiting for a begun frame (the throttle it
// sees); the others time the calls themselves.
enum StatId {
    kFrameWait, kWaitFrame, kBeginFrame, kAcquire, kWaitImage, kCopy, kRelease, kEndFrame, kLockWait, kStatCount
};
const char *const kStatName[kStatCount] = {"frameWait (render)", "waitFrame", "beginFrame", "acquire", "waitImage",
                                           "copySubmit", "release", "endFrame", "queueLockWait"};
Stat w_stat[kStatCount];
struct Timed {
    int id;
    int64_t t0;  // QPC ticks (qpc.hpp)
    explicit Timed(int i) : id(i), t0(qpc::now()) {}
    ~Timed() { w_stat[id].add(qpc::ms_since(t0)); }
};

// DXVK's queue lock for the session's runtime calls; the time to get it
// (DXVK draining its submission queue, or its submit thread holding it) is
// measured.
struct QLock {
    QLock()
    {
        const int64_t t0 = qpc::now();
        g_interop->LockSubmissionQueue();
        w_stat[kLockWait].add(qpc::ms_since(t0));
    }
    ~QLock() { g_interop->ReleaseSubmissionQueue(); }
    QLock(const QLock &) = delete;
    QLock &operator=(const QLock &) = delete;
};

const char *result_name(XrResult r)
{
    static thread_local char buf[XR_MAX_RESULT_STRING_SIZE];  // both threads log failures
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

// Publishes a begun frame's views (render thread; they carry the session
// state of their frame), or just the session state (frame thread, on events).
void publish_state(const ViewState *views = nullptr)
{
    AcquireSRWLockExclusive(&g_state_lock);
    if (views != nullptr) {
        g_state = *views;
    } else {
        g_state.session_state = (uint32_t)g_xr_state;
    }
    ReleaseSRWLockExclusive(&g_state_lock);
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
    if (FAILED(g_d3d.load()->QueryInterface(dxvk::kIID_InteropDevice, reinterpret_cast<void **>(&g_interop))) ||
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
    bool has_vk = false, has_qpc = false;
    if (XR_SUCCEEDED(xr.xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr)) && n > 0) {
        std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
        xr.xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data());
        for (uint32_t i = 0; i < n; i++) {
            has_vk = has_vk || strcmp(props[i].extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0;
            has_qpc = has_qpc ||
                      strcmp(props[i].extensionName, XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME) == 0;
        }
    }
    if (!has_vk) {
        MC2VR_LOG("xr: runtime lacks " XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
        return false;
    }

    // The QPC -> XrTime conversion only feeds the latency log (optional).
    const char *exts[] = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME,
                          XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME};
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ici.applicationInfo.applicationName, "Mercenaries 2 (mc2vr)");
    ici.applicationInfo.applicationVersion = 1;
    strcpy(ici.applicationInfo.engineName, "mc2vr");
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ici.enabledExtensionCount = has_qpc ? 2 : 1;
    ici.enabledExtensionNames = exts;
    if (!ok(xr.xrCreateInstance(&ici, &g_inst), "xrCreateInstance")) {
        return false;
    }
    if (!resolve(g_inst)) {
        return false;
    }
    if (has_qpc) {
        g_gipa(g_inst, "xrConvertWin32PerformanceCounterToTimeKHR",
               reinterpret_cast<PFN_xrVoidFunction *>(&g_qpc_to_xr));
    }
    MC2VR_LOG("xr: %s", g_qpc_to_xr ? "QPC -> XrTime conversion available (latency clock candidate)"
                                     : "no " XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);
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
        QLock lock;
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
    QLock lock;
    for (Eye &e : g_eye) {
        if (e.swapchain != XR_NULL_HANDLE) xr.xrDestroySwapchain(e.swapchain);
        e.swapchain = XR_NULL_HANDLE;
        e.images.clear();
    }
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
    for (int e = 0; e < 2; e++) {
        char list[256] = "";
        size_t len = 0;
        for (const auto &img : g_eye[e].images) {
            len += snprintf(list + len, sizeof(list) - len, " 0x%llx", (unsigned long long)img.image);
        }
        MC2VR_LOG("xr: swapchain %s VkImage:%s", e ? "R" : "L", list);
    }
    return true;
}

// Acquire with a successful wait (the only state an image may be released
// from). Waits retry on XR_TIMEOUT_EXPIRED, as the spec requires.
bool acquire_and_wait(Eye &ey, uint32_t *idx, bool *acquired)
{
    {
        QLock lock;
        Timed t(kAcquire);
        *acquired = ok(xr.xrAcquireSwapchainImage(ey.swapchain, nullptr, idx), "xrAcquireSwapchainImage");
    }
    if (!*acquired || *idx >= ey.images.size()) {
        return false;
    }
    for (int attempt = 0; attempt < 50; attempt++) {
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = 100ll * 1000 * 1000;
        XrResult r;
        {
            Timed t(kWaitImage);
            r = xr.xrWaitSwapchainImage(ey.swapchain, &wi);
        }
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
        Timed t(kCopy);
        good = xr_present::copy_pair(p.eye, dst, p.w, p.h);
    }
    // Released either way (a failed copy leaves the image's old contents).
    // An acquired image whose wait failed can't be released; the swapchain
    // is re-created instead.
    bool stuck = false;
    {
        QLock lock;
        for (int e = 0; e < 2; e++) {
            if (waited[e]) {
                Timed t(kRelease);
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
    QLock lock;
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

// Frame thread only.
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
                QLock lock;
                g_running = ok(xr.xrBeginSession(g_session, &bi), "xrBeginSession");
            } else if (sc.state == XR_SESSION_STATE_STOPPING) {
                g_stop_pending = true;  // xrEndSession once no frame is open (frame thread)
            } else if (sc.state == XR_SESSION_STATE_EXITING || sc.state == XR_SESSION_STATE_LOSS_PENDING) {
                request_game_exit("the runtime ended the session");
                g_exiting = true;
            }
            publish_state();
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            request_game_exit("the runtime is going away (instance loss pending)");
            g_exiting = true;
            break;
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
            // Nothing to apply: the camera follows each frame's views, so a
            // recenter shows at the next frame.
            MC2VR_LOG("xr: reference space change pending (recenter)");
            break;
        default:
            break;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// ---- the frame ------------------------------------------------------------------------

// Render thread.
void report(ULONGLONG now)
{
    if (w_next == 0) {
        w_next = now + 10000;
        return;
    }
    if (now < w_next) {
        return;
    }
    w_next = now + 10000;
    MC2VR_LOG("submit: frames=%llu (~%.0f Hz) shown=%llu hidden=%llu noFrame=%llu | no layer: noPair=%llu "
              "noPose=%llu copyFail=%llu layerRejected=%llu",
              (unsigned long long)w_frames, w_frames / 10.0, (unsigned long long)w_shown,
              (unsigned long long)w_hidden, (unsigned long long)w_no_frame, (unsigned long long)w_no_pair,
              (unsigned long long)w_no_pose, (unsigned long long)w_copy_fail, (unsigned long long)w_end_layer_fail);
    char line[640];
    size_t len = 0;
    AcquireSRWLockExclusive(&g_stat_lock);
    if (w_pose_n != 0) {
        MC2VR_LOG("submit: latency pose->display avg=%.2fms max=%.2fms | endFrame->display avg=%.2fms min=%.2fms "
                  "| prediction depth (periods) 1:%llu 2:%llu 3:%llu 4+:%llu",
                  w_pose_sum / (double)w_pose_n, w_pose_max, w_margin_n ? w_margin_sum / (double)w_margin_n : 0.0,
                  w_margin_min, (unsigned long long)w_depth[1], (unsigned long long)w_depth[2],
                  (unsigned long long)w_depth[3], (unsigned long long)w_depth[4]);
    }
    for (int i = 0; i < kStatCount; i++) {
        len += snprintf(line + len, sizeof(line) - len, "%s%s %.2f/%.2f", i ? " | " : "", kStatName[i],
                        w_stat[i].avg(), w_stat[i].max);
        w_stat[i] = Stat{};
    }
    w_margin_sum = w_margin_min = 0.0;
    w_margin_n = w_end_layer_fail = 0;
    ReleaseSRWLockExclusive(&g_stat_lock);
    MC2VR_LOG("submit: calls avg/max ms: %s", line);
    w_frames = w_shown = w_hidden = w_no_pair = w_no_pose = w_copy_fail = w_no_frame = 0;
    w_pose_sum = w_pose_max = 0.0;
    w_pose_n = 0;
    for (uint64_t &d : w_depth) d = 0;
}

XrPosef to_xr_pose(const EyePose &e)
{
    XrPosef p;
    p.orientation = {e.rot.x, e.rot.y, e.rot.z, e.rot.w};
    p.position = {e.pos.x, e.pos.y, e.pos.z};
    return p;
}

XrFovf to_xr_fov(const EyePose &e)
{
    return {e.fov.left, e.fov.right, e.fov.up, e.fov.down};
}

// Render thread, with the frame thread gone (or never started).
void teardown()
{
    AcquireSRWLockExclusive(&g_state_lock);
    g_active = false;
    ReleaseSRWLockExclusive(&g_state_lock);
    if (g_session != XR_NULL_HANDLE && g_interop != nullptr) {
        if (xr.xrDestroySwapchain) destroy_swapchains();
        if (g_space != XR_NULL_HANDLE && xr.xrDestroySpace) xr.xrDestroySpace(g_space);
        QLock lock;
        if (g_running && xr.xrEndSession) xr.xrEndSession(g_session);
        if (xr.xrDestroySession) xr.xrDestroySession(g_session);
    }
    if (g_inst != XR_NULL_HANDLE && xr.xrDestroyInstance) xr.xrDestroyInstance(g_inst);
    g_session = XR_NULL_HANDLE;
    g_space = XR_NULL_HANDLE;
    g_inst = XR_NULL_HANDLE;
    g_running = false;
    g_setup = Setup::Off;
    MC2VR_LOG("xr: session ended — VR off for the rest of the run");
}

// Frame thread: ends the open frame with the render thread's layer (or none).
void end_open_frame(XrTime display_time, bool has_layer, const XrCompositionLayerProjectionView pv[2])
{
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = g_space;
    layer.viewCount = 2;
    layer.views = pv;
    for (int attempt = 0; attempt < 2; attempt++) {
        const bool with_layer = has_layer && attempt == 0;
        const XrCompositionLayerBaseHeader *layers[1] = {
            reinterpret_cast<const XrCompositionLayerBaseHeader *>(&layer)};
        XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
        ei.displayTime = display_time;
        ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        ei.layerCount = with_layer ? 1 : 0;
        ei.layers = with_layer ? layers : nullptr;
        XrResult r;
        {
            QLock lock;
            Timed t(kEndFrame);
            r = xr.xrEndFrame(g_session, &ei);
        }
        if (XR_SUCCEEDED(r)) {
            g_frame_errors = 0;
            if (with_layer) {
                const XrTime now = xr_now(display_time);
                if (now != 0) {
                    const double margin_ms = (double)(display_time - now) / 1e6;
                    AcquireSRWLockExclusive(&g_stat_lock);
                    w_margin_min = w_margin_n == 0 || margin_ms < w_margin_min ? margin_ms : w_margin_min;
                    w_margin_sum += margin_ms;
                    w_margin_n++;
                    ReleaseSRWLockExclusive(&g_stat_lock);
                }
            }
            return;
        }
        if (with_layer) {
            // A rejected layer: the frame is still begun, end it without.
            ok(r, "xrEndFrame (layer dropped)");
            AcquireSRWLockExclusive(&g_stat_lock);
            w_end_layer_fail++;
            ReleaseSRWLockExclusive(&g_stat_lock);
            continue;
        }
        ok(r, "xrEndFrame");
        g_frame_errors++;
        return;
    }
}

// Frame thread: xrEndSession once STOPPING arrived and no frame is open.
void end_session_if_stopping()
{
    if (!g_stop_pending) return;
    g_stop_pending = false;
    {
        QLock lock;
        ok(xr.xrEndSession(g_session), "xrEndSession");
    }
    g_running = false;
    WakeAllConditionVariable(&g_hand_cv);  // a render thread waiting for a frame stops waiting
}

// The frame thread: the runtime's session events and frame calls, one frame in
// flight, each frame ended with the layer the render thread hands back.
DWORD WINAPI frame_thread(void *)
{
    uint32_t serial = 0;
    for (;;) {
        handle_events();
        end_session_if_stopping();
        if (g_exiting || g_frame_errors > 100) {
            if (!g_exiting) MC2VR_LOG("xr: frame calls keep failing — ending the session");
            break;
        }
        if (!g_running) {
            Sleep(10);
            continue;
        }

        XrFrameState fs{XR_TYPE_FRAME_STATE};
        XrResult r;
        {
            Timed t(kWaitFrame);
            r = xr.xrWaitFrame(g_session, nullptr, &fs);
        }
        if (!ok(r, "xrWaitFrame")) {
            g_frame_errors++;
            Sleep(1);
            continue;
        }
        {
            QLock lock;
            Timed t(kBeginFrame);
            r = xr.xrBeginFrame(g_session, nullptr);
        }
        if (!ok(r, "xrBeginFrame")) {
            g_frame_errors++;
            Sleep(1);
            continue;
        }

        // Offer the frame to the render thread and wait for its layer. The
        // render thread may never take it (the game isn't rendering): keep
        // handling events meanwhile, and end it empty when the session stops.
        EnterCriticalSection(&g_hand_lock);
        g_hand.serial = ++serial;
        g_hand.fs = fs;
        g_hand.session_state = (uint32_t)g_xr_state;
        g_hand.available = true;
        g_hand.claimed = false;
        g_hand.end_ready = false;
        LeaveCriticalSection(&g_hand_lock);
        WakeAllConditionVariable(&g_hand_cv);

        bool has_layer = false;
        XrCompositionLayerProjectionView pv[2];
        int stop_ticks = 0;
        for (;;) {
            EnterCriticalSection(&g_hand_lock);
            if (!(g_hand.end_ready && g_hand.end_serial == serial)) {
                SleepConditionVariableCS(&g_hand_cv, &g_hand_lock, 10);
            }
            const bool ended = g_hand.end_ready && g_hand.end_serial == serial;
            if (ended) {
                has_layer = g_hand.has_layer;
                pv[0] = g_hand.pv[0];
                pv[1] = g_hand.pv[1];
            }
            // Withdraw the frame when stopping and the render thread hasn't
            // taken it; a taken one gets a bounded wait for its layer.
            const bool stopping = g_exiting || g_stop_pending;
            const bool give_up = !ended && stopping && !g_hand.presenting &&
                                 (!g_hand.claimed || ++stop_ticks > kEndWaitTicks);
            if (ended || give_up) {
                g_hand.available = g_hand.claimed = g_hand.end_ready = false;
            }
            LeaveCriticalSection(&g_hand_lock);
            if (ended || give_up) {
                break;
            }
            handle_events();
        }
        end_open_frame(fs.predictedDisplayTime, has_layer, pv);
    }
    EnterCriticalSection(&g_hand_lock);
    g_hand.stopped = true;
    g_hand.available = false;
    LeaveCriticalSection(&g_hand_lock);
    WakeAllConditionVariable(&g_hand_cv);
    MC2VR_LOG("xr: frame thread done");
    return 0;
}

} // namespace

void start(IDirect3DDevice9 *device)
{
    g_d3d.store(device);
}

Frame begin_frame()
{
    // The render thread runs the x87 at single precision (D3D9); the runtime's
    // timing math and ours need full precision (fpu_precision.hpp).
    const FullFpuPrecision fpu;
    static unsigned fpu_seen = ~0u;  // logged on every change (DXVK re-applies it at Reset)
    if (fpu.saved_precision() != fpu_seen) {
        fpu_seen = fpu.saved_precision();
        MC2VR_LOG("xr: render thread x87 precision on entry: %s (full precision inside the OpenXR calls)",
                  fpu_seen == 0 ? "24-bit (D3D9 single)" : fpu_seen == 2 ? "53-bit" : "64-bit");
    }
    Frame f;
    if (g_setup == Setup::NotYet) {
        IDirect3DDevice9 *dev = g_d3d.load();
        if (dev == nullptr) {
            return f;  // before the first Present
        }
        InitializeCriticalSection(&g_hand_lock);
        g_setup = setup() ? Setup::Ready : Setup::Off;
        if (g_setup == Setup::Ready) {
            g_frame_thread = CreateThread(nullptr, 0, frame_thread, nullptr, 0, nullptr);
            if (g_frame_thread == nullptr) {
                MC2VR_LOG("xr: CreateThread(frame thread) failed (%lu)", GetLastError());
                g_setup = Setup::Off;
            }
        }
        if (g_setup == Setup::Off) {
            MC2VR_LOG("xr: no OpenXR session — VR off for this run (camera pass-through)");
            teardown();
        }
    }
    if (g_setup != Setup::Ready) {
        return f;
    }

    EnterCriticalSection(&g_hand_lock);
    if (g_claimed_serial != 0 && g_hand.claimed && g_hand.serial == g_claimed_serial && !g_hand.end_ready) {
        // The previous frame never reached end_frame (a pass aborted): hand it
        // back empty, as the frame loop requires.
        g_hand.end_ready = true;
        g_hand.end_serial = g_claimed_serial;
        g_hand.has_layer = false;
        WakeAllConditionVariable(&g_hand_cv);
    }
    g_claimed_serial = 0;
    if (g_hand.stopped) {
        LeaveCriticalSection(&g_hand_lock);
        WaitForSingleObject(g_frame_thread, INFINITE);
        CloseHandle(g_frame_thread);
        g_frame_thread = nullptr;
        teardown();
        return f;
    }
    // Wait for the next begun frame: SteamVR's throttle, from the render
    // thread's side. Bounded, so a stuck runtime can't hang the game.
    bool got = false;
    if (g_begin_timed_out) {
        // The frame thread is stuck or slow (a runtime stall): take a frame
        // only if one is offered, instead of paying the full wait each frame.
        got = g_hand.available && !g_hand.claimed && !g_hand.end_ready;
        g_begin_timed_out = !got;
    } else if (g_running) {
        const int64_t t0 = qpc::now();
        for (;;) {
            got = g_hand.available && !g_hand.claimed && !g_hand.end_ready;
            const double left = (double)kBeginWaitMs - qpc::ms_since(t0);
            if (got || g_hand.stopped || !g_running) break;
            if (left <= 0.0) {
                g_begin_timed_out = true;
                break;
            }
            SleepConditionVariableCS(&g_hand_cv, &g_hand_lock, (DWORD)left + 1);
        }
        w_stat[kFrameWait].add(qpc::ms_since(t0));
    }
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    uint32_t state = 0;
    if (got) {
        g_hand.claimed = true;
        g_claimed_serial = g_hand.serial;
        fs = g_hand.fs;
        state = g_hand.session_state;
    }
    LeaveCriticalSection(&g_hand_lock);
    if (!got) {
        w_no_frame += g_running ? 1 : 0;
        return f;
    }

    f.begun = true;
    f.serial = g_claimed_serial;
    w_frames++;
    ViewState &v = f.views;
    v.pose_id = ++g_frame_id;
    v.display_time = fs.predictedDisplayTime;
    v.display_period = fs.predictedDisplayPeriod;
    v.session_state = state;
    if (fs.shouldRender) {
        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
        li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        li.displayTime = fs.predictedDisplayTime;
        li.space = g_space;
        XrViewState vs{XR_TYPE_VIEW_STATE};
        XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t nv = 0;
        if (ok(xr.xrLocateViews(g_session, &li, &vs, 2, &nv, views), "xrLocateViews") && nv == 2) {
            v.tracked = (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
                        (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
            v.eye[0] = to_eye(views[0]);
            v.eye[1] = to_eye(views[1]);
        }
        const XrTime now = xr_now(fs.predictedDisplayTime);
        if (now != 0) {
            const double pose_ms = (double)(fs.predictedDisplayTime - now) / 1e6;
            AcquireSRWLockExclusive(&g_stat_lock);
            w_pose_sum += pose_ms;
            w_pose_max = pose_ms > w_pose_max ? pose_ms : w_pose_max;
            w_pose_n++;
            if (fs.predictedDisplayPeriod > 0) {
                int depth = (int)(pose_ms * 1e6 / (double)fs.predictedDisplayPeriod + 0.5);
                w_depth[depth < 0 ? 0 : depth > 4 ? 4 : depth]++;
            }
            ReleaseSRWLockExclusive(&g_stat_lock);
        }
    }
    f.show = fs.shouldRender && lockstep(state);
    publish_state(&v);
    return f;
}

void end_frame(const Frame &frame, const ViewState *rendered, const eye_share::Pair *pair)
{
    const FullFpuPrecision fpu;  // see begin_frame
    if (!frame.begun || frame.serial != g_claimed_serial) {
        return;
    }
    // The layer's views: the ones both passes rendered with; without them
    // (a debug camera mode), this frame's own if valid.
    const ViewState *views = rendered != nullptr ? rendered : frame.views.tracked ? &frame.views : nullptr;
    bool layer_ok = false;
    if (!frame.show) {
        w_hidden++;
    } else if (pair == nullptr) {
        w_no_pair++;  // no eye images this frame (transition frame after a Reset)
    } else if (views == nullptr) {
        w_no_pose++;
    } else if (pair->device != g_d3d.load()) {
        if (!g_wrong_device_logged) {
            g_wrong_device_logged = true;
            MC2VR_LOG("xr: the game re-created its D3D9 device — the session is bound to the old one; "
                      "nothing more is shown");
        }
    } else if (ensure_swapchains(pair->w, pair->h, pair->format)) {
        // Only while the frame is still ours (a stopping session withdraws a
        // frame that took too long), and not withdrawable meanwhile.
        EnterCriticalSection(&g_hand_lock);
        const bool ours = g_hand.claimed && g_hand.serial == frame.serial;
        g_hand.presenting = ours;
        LeaveCriticalSection(&g_hand_lock);
        if (ours) {
            layer_ok = present_pair(*pair);  // acquire, copy on DXVK's queue, release
            w_copy_fail += layer_ok ? 0 : 1;
            EnterCriticalSection(&g_hand_lock);
            g_hand.presenting = false;
            LeaveCriticalSection(&g_hand_lock);
        }
    }
    w_shown += layer_ok ? 1 : 0;

    // Hand the layer to the frame thread, which ends the frame (and waits out
    // SteamVR's throttle there) while this thread goes on.
    EnterCriticalSection(&g_hand_lock);
    if (g_hand.claimed && g_hand.serial == frame.serial) {
        g_hand.has_layer = layer_ok;
        if (layer_ok) {
            for (int e = 0; e < 2; e++) {
                XrCompositionLayerProjectionView &pv = g_hand.pv[e];
                pv = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                pv.pose = to_xr_pose(views->eye[e]);
                pv.fov = to_xr_fov(views->eye[e]);
                pv.subImage.swapchain = g_eye[e].swapchain;
                pv.subImage.imageRect = {{0, 0}, {(int32_t)g_sc_w, (int32_t)g_sc_h}};
            }
        }
        g_hand.end_serial = frame.serial;
        g_hand.end_ready = true;
    }
    g_claimed_serial = 0;
    LeaveCriticalSection(&g_hand_lock);
    WakeAllConditionVariable(&g_hand_cv);
    report(GetTickCount64());
}

bool read_state(ViewState *out)
{
    AcquireSRWLockShared(&g_state_lock);
    const bool active = g_active;
    if (active) *out = g_state;
    ReleaseSRWLockShared(&g_state_lock);
    return active;
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
