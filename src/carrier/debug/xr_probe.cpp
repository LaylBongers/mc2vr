// debug_xr_probe — see xr_probe.hpp.

#include "debug/xr_probe.hpp"

#include <windows.h>
#include <d3d9.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

// ---- minimal Vulkan types for openxr_platform.h's Vulkan section ----------
// The i686 toolchain has no Vulkan headers, and the probe makes no Vulkan
// calls: it only passes DXVK's handles through. Handles, enums (int-sized),
// flag typedefs and pointer-only structs, as vulkan_core.h defines them for a
// 32-bit target (non-dispatchable handles are uint64_t there).
#define VKAPI_PTR __stdcall
typedef struct VkInstance_T *VkInstance;
typedef struct VkPhysicalDevice_T *VkPhysicalDevice;
typedef struct VkDevice_T *VkDevice;
typedef struct VkQueue_T *VkQueue;
typedef uint64_t VkImage;
typedef uint32_t VkFlags;
typedef VkFlags VkImageCreateFlags;
typedef VkFlags VkImageUsageFlags;
typedef enum VkResult { VK_RESULT_PROBE_MAX_ENUM = 0x7fffffff } VkResult;
typedef enum VkFormat { VK_FORMAT_PROBE_MAX_ENUM = 0x7fffffff } VkFormat;
typedef enum VkFilter { VK_FILTER_PROBE_MAX_ENUM = 0x7fffffff } VkFilter;
typedef enum VkSamplerMipmapMode { VK_SAMPLER_MIPMAP_MODE_PROBE_MAX_ENUM = 0x7fffffff } VkSamplerMipmapMode;
typedef enum VkSamplerAddressMode { VK_SAMPLER_ADDRESS_MODE_PROBE_MAX_ENUM = 0x7fffffff } VkSamplerAddressMode;
typedef enum VkComponentSwizzle { VK_COMPONENT_SWIZZLE_PROBE_MAX_ENUM = 0x7fffffff } VkComponentSwizzle;
typedef struct VkAllocationCallbacks VkAllocationCallbacks;
typedef struct VkInstanceCreateInfo VkInstanceCreateInfo;
typedef struct VkDeviceCreateInfo VkDeviceCreateInfo;
typedef void(VKAPI_PTR *PFN_vkVoidFunction)(void);
typedef PFN_vkVoidFunction(VKAPI_PTR *PFN_vkGetInstanceProcAddr)(VkInstance, const char *);

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include "ipc.hpp"
#include "log.hpp"
#include "vr/render_outputs.hpp"

// ---- DXVK D3D9 interop: external linkage on purpose ------------------------
// Declared outside any anonymous namespace: GCC sees every class with internal
// linkage, finds no concrete subclass of these abstract interfaces, and
// compiles virtual calls through them as unreachable (it emitted `call 0`;
// the 2026-11-10 probe run faulted executing data at 0x09390000).
namespace mc2vr::xr_probe::dxvk {

// ---- DXVK D3D9 interop (DXVK's src/d3d9/d3d9_interfaces.h) -----------------
// Only the methods the probe calls, plus the slots before them.
struct ID3D9VkInteropInterface : public IUnknown {
    virtual void STDMETHODCALLTYPE GetInstanceHandle(VkInstance *instance) = 0;
    virtual void STDMETHODCALLTYPE GetPhysicalDeviceHandle(UINT adapter, VkPhysicalDevice *phys) = 0;
};
struct ID3D9VkInteropInterface1 : public ID3D9VkInteropInterface {
    // D3DERR_MOREDATA if the list was truncated.
    virtual HRESULT STDMETHODCALLTYPE GetInstanceExtensions(UINT *count, const char **names) = 0;
};
struct ID3D9VkInteropDevice : public IUnknown {
    virtual void STDMETHODCALLTYPE GetVulkanHandles(VkInstance *instance, VkPhysicalDevice *phys,
                                                    VkDevice *device) = 0;
    virtual void STDMETHODCALLTYPE GetSubmissionQueue(VkQueue *queue, uint32_t *index,
                                                      uint32_t *family) = 0;
    virtual void STDMETHODCALLTYPE TransitionTextureLayout(void *, const void *, int, int) = 0;  // slot only
    virtual void STDMETHODCALLTYPE FlushRenderingCommands() = 0;
    virtual void STDMETHODCALLTYPE LockSubmissionQueue() = 0;
    virtual void STDMETHODCALLTYPE ReleaseSubmissionQueue() = 0;
};
inline constexpr GUID kIID_InteropInterface1 = {0xd6589ed4, 0x7a37, 0x4096,
                                         {0xba, 0xc2, 0x22, 0x3b, 0x25, 0xae, 0x31, 0xd2}};
inline constexpr GUID kIID_InteropDevice = {0x2eaa4b89, 0x0107, 0x4bdb,
                                     {0x87, 0xf7, 0x0f, 0x54, 0x1c, 0x49, 0x3c, 0xe0}};

} // namespace mc2vr::xr_probe::dxvk

namespace mc2vr::xr_probe {

namespace {

using namespace dxvk;

bool g_enabled = false;
bool g_started = false;

// ---- the runtime's entry points (resolved through getInstanceProcAddr) ----
PFN_xrGetInstanceProcAddr g_gipa = nullptr;
XrInstance g_inst = XR_NULL_HANDLE;

template <class F>
F proc(const char *name)
{
    PFN_xrVoidFunction fn = nullptr;
    if (g_gipa == nullptr || XR_FAILED(g_gipa(g_inst, name, &fn))) {
        return nullptr;
    }
    return reinterpret_cast<F>(fn);
}

const char *result_name(XrResult r)
{
    static char buf[XR_MAX_RESULT_STRING_SIZE];
    auto to_string = proc<PFN_xrResultToString>("xrResultToString");
    if (g_inst != XR_NULL_HANDLE && to_string != nullptr && XR_SUCCEEDED(to_string(g_inst, r, buf))) {
        return buf;
    }
    snprintf(buf, sizeof(buf), "XrResult %d", (int)r);
    return buf;
}

// Logs a step's result; true = it succeeded.
bool step(const char *what, XrResult r)
{
    MC2VR_LOG("xrprobe: %s -> %s", what, result_name(r));
    return XR_SUCCEEDED(r);
}

// Splits a space-separated extension list and checks each against `have`.
void check_extensions(const char *kind, const char *list, const char *const *have, UINT have_n)
{
    MC2VR_LOG("xrprobe: runtime requires %s extensions: %s", kind, list);
    if (have == nullptr) {
        return;
    }
    const char *p = list;
    uint32_t missing = 0;
    while (*p != '\0') {
        while (*p == ' ') p++;
        const char *e = p;
        while (*e != '\0' && *e != ' ') e++;
        if (e > p) {
            bool found = false;
            for (UINT i = 0; i < have_n && !found; i++) {
                found = strlen(have[i]) == (size_t)(e - p) && strncmp(have[i], p, e - p) == 0;
            }
            if (!found) {
                MC2VR_LOG("xrprobe:   MISSING in DXVK's %s: %.*s", kind, (int)(e - p), p);
                missing++;
            }
        }
        p = e;
    }
    MC2VR_LOG("xrprobe: %s extensions: %u required ones missing from DXVK", kind, missing);
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
    default: return "?";
    }
}

// Polls events until the session reaches `want` (true) or timeout_ms passes.
bool wait_state(XrSession session, XrSessionState want, DWORD timeout_ms, XrSessionState *last)
{
    auto poll = proc<PFN_xrPollEvent>("xrPollEvent");
    const DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < timeout_ms) {
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        const XrResult r = poll(g_inst, &ev);
        if (r == XR_EVENT_UNAVAILABLE) {
            Sleep(10);
            continue;
        }
        if (XR_FAILED(r)) {
            step("xrPollEvent", r);
            return false;
        }
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto &sc = *reinterpret_cast<const XrEventDataSessionStateChanged *>(&ev);
            if (sc.session == session) {
                *last = sc.state;
                MC2VR_LOG("xrprobe: session state -> %s (+%lu ms)", state_name(sc.state),
                          (unsigned long)(GetTickCount() - t0));
                if (sc.state == want) {
                    return true;
                }
            }
        } else {
            MC2VR_LOG("xrprobe: event type %d", (int)ev.type);
        }
    }
    return false;
}

void run_session(ID3D9VkInteropDevice *interop, VkInstance vk_inst, VkPhysicalDevice vk_phys,
                 VkDevice vk_dev, VkQueue vk_queue, uint32_t q_index, uint32_t q_family,
                 XrSystemId system)
{
    (void)vk_queue;
    XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    binding.instance = vk_inst;
    binding.physicalDevice = vk_phys;
    binding.device = vk_dev;
    binding.queueFamilyIndex = q_family;
    binding.queueIndex = q_index;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = system;
    XrSession session = XR_NULL_HANDLE;
    interop->LockSubmissionQueue();
    const XrResult cr = proc<PFN_xrCreateSession>("xrCreateSession")(g_inst, &sci, &session);
    interop->ReleaseSubmissionQueue();
    if (!step("xrCreateSession (DXVK device + queue)", cr)) {
        return;
    }

    XrSessionState last = XR_SESSION_STATE_UNKNOWN;
    if (!wait_state(session, XR_SESSION_STATE_READY, 10000, &last)) {
        MC2VR_LOG("xrprobe: session never reached READY (last %s)", state_name(last));
        step("xrDestroySession", proc<PFN_xrDestroySession>("xrDestroySession")(session));
        return;
    }
    MC2VR_LOG("xrprobe: RESULT session READY on DXVK's device");

    XrSessionBeginInfo sbi{XR_TYPE_SESSION_BEGIN_INFO};
    sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    interop->LockSubmissionQueue();
    const XrResult br = proc<PFN_xrBeginSession>("xrBeginSession")(session, &sbi);
    interop->ReleaseSubmissionQueue();
    if (step("xrBeginSession", br)) {
        XrReferenceSpaceCreateInfo rsi{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        rsi.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        rsi.poseInReferenceSpace.orientation.w = 1.0f;
        XrSpace space = XR_NULL_HANDLE;
        step("xrCreateReferenceSpace(LOCAL)",
             proc<PFN_xrCreateReferenceSpace>("xrCreateReferenceSpace")(session, &rsi, &space));

        auto wait = proc<PFN_xrWaitFrame>("xrWaitFrame");
        auto begin = proc<PFN_xrBeginFrame>("xrBeginFrame");
        auto end = proc<PFN_xrEndFrame>("xrEndFrame");
        auto locate = proc<PFN_xrLocateViews>("xrLocateViews");
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        constexpr int kFrames = 180;
        int done = 0, failed = 0, rendered = 0;
        XrTime first_time = 0, last_time = 0;
        int64_t period_ns = 0;
        QueryPerformanceCounter(&t0);
        for (int i = 0; i < kFrames; i++) {
            XrFrameState fs{XR_TYPE_FRAME_STATE};
            XrResult r = wait(session, nullptr, &fs);
            if (XR_SUCCEEDED(r)) r = begin(session, nullptr);
            if (XR_SUCCEEDED(r) && i == kFrames / 2 && space != XR_NULL_HANDLE) {
                XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
                li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                li.displayTime = fs.predictedDisplayTime;
                li.space = space;
                XrViewState vs{XR_TYPE_VIEW_STATE};
                XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
                uint32_t n = 0;
                if (step("xrLocateViews", locate(session, &li, &vs, 2, &n, views)) && n == 2) {
                    MC2VR_LOG("xrprobe: view0 pos (%.3f %.3f %.3f) fov L%.3f R%.3f U%.3f D%.3f flags 0x%llx",
                              views[0].pose.position.x, views[0].pose.position.y,
                              views[0].pose.position.z, views[0].fov.angleLeft,
                              views[0].fov.angleRight, views[0].fov.angleUp, views[0].fov.angleDown,
                              (unsigned long long)vs.viewStateFlags);
                }
            }
            if (XR_SUCCEEDED(r)) {
                XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
                ei.displayTime = fs.predictedDisplayTime;
                ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                ei.layerCount = 0;
                interop->LockSubmissionQueue();
                r = end(session, &ei);
                interop->ReleaseSubmissionQueue();
            }
            if (XR_FAILED(r)) {
                if (failed++ == 0) step("frame loop (first failure)", r);
                continue;
            }
            if (done == 0) first_time = fs.predictedDisplayTime;
            last_time = fs.predictedDisplayTime;
            period_ns = fs.predictedDisplayPeriod;
            rendered += fs.shouldRender ? 1 : 0;
            done++;
        }
        QueryPerformanceCounter(&t1);
        const double wall_ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
        MC2VR_LOG("xrprobe: RESULT frame loop: %d/%d frames ok (%d failed, shouldRender %d), "
                  "wall %.2f ms/frame, predictedDisplayPeriod %.2f ms, display-time step %.2f ms",
                  done, kFrames, failed, rendered, wall_ms / kFrames, (double)period_ns / 1e6,
                  done > 1 ? (double)(last_time - first_time) / 1e6 / (done - 1) : 0.0);
        if (space != XR_NULL_HANDLE) proc<PFN_xrDestroySpace>("xrDestroySpace")(space);

        step("xrRequestExitSession", proc<PFN_xrRequestExitSession>("xrRequestExitSession")(session));
        if (wait_state(session, XR_SESSION_STATE_STOPPING, 5000, &last)) {
            step("xrEndSession", proc<PFN_xrEndSession>("xrEndSession")(session));
        } else {
            MC2VR_LOG("xrprobe: no STOPPING after the exit request (last %s)", state_name(last));
        }
    }
    step("xrDestroySession", proc<PFN_xrDestroySession>("xrDestroySession")(session));
}

// Fault log while the probe is enabled: the first few fatal-kind exceptions
// in ANY thread, first chance (before any handler decides), with the faulting
// module + offset. Wine and the game may handle some of these themselves, so a
// logged fault is a lead, not necessarily the crash; the last one before the
// log ends usually is.
LONG CALLBACK fault_log(EXCEPTION_POINTERS *ep)
{
    static volatile LONG logged = 0;
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_PRIV_INSTRUCTION && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_STACK_OVERFLOW) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (InterlockedIncrement(&logged) > 16) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    void *const at = ep->ExceptionRecord->ExceptionAddress;
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)at, &mod) && mod != nullptr) {
        GetModuleFileNameA(mod, name, sizeof(name));
    }
    const char *base = strrchr(name, '\\');
    MC2VR_LOG("xrprobe: FAULT %08lx at %p (%s+0x%lx) thread %lu, access %s %p", (unsigned long)code, at,
              base ? base + 1 : name, (unsigned long)((uintptr_t)at - (uintptr_t)mod),
              (unsigned long)GetCurrentThreadId(),
              ep->ExceptionRecord->NumberParameters >= 2
                  ? (ep->ExceptionRecord->ExceptionInformation[0] == 0 ? "read"
                     : ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "write" : "exec")
                  : "-",
              ep->ExceptionRecord->NumberParameters >= 2 ? (void *)ep->ExceptionRecord->ExceptionInformation[1]
                                                         : nullptr);
    return EXCEPTION_CONTINUE_SEARCH;
}

// "module+0xoffset" for a code/data address (probe logs).
void describe(const void *at, char *out, size_t n)
{
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)at, &mod) && mod != nullptr) {
        GetModuleFileNameA(mod, name, sizeof(name));
        const char *b = strrchr(name, '\\');
        snprintf(out, n, "%s+0x%lx", b ? b + 1 : name, (unsigned long)((uintptr_t)at - (uintptr_t)mod));
    } else {
        snprintf(out, n, "%p(no module)", at);
    }
}

// Logs an interface object's vtable (slots 0..count-1) and returns true when
// every slot points into d3d9.dll — i.e. calling through it can't jump into
// data. The 2026-11-10 run faulted executing 0x09390000 at slot 3.
bool vtable_in_d3d9(const char *what, const void *obj, int count)
{
    const HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
    const void *const *vt = *reinterpret_cast<const void *const *const *>(obj);
    char desc[160];
    describe(vt, desc, sizeof(desc));
    MC2VR_LOG("xrprobe: %s %p vtable %p (%s), d3d9.dll @ %p", what, obj, (const void *)vt, desc, (void *)d3d9);
    bool ok = d3d9 != nullptr;
    for (int i = 0; i < count; i++) {
        HMODULE m = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)vt[i], &m);
        describe(vt[i], desc, sizeof(desc));
        MC2VR_LOG("xrprobe:   slot %d = %p (%s)", i, vt[i], desc);
        ok = ok && m == d3d9;
    }
    return ok;
}

DWORD WINAPI probe_thread(void *)
{
    MC2VR_LOG("xrprobe: thread %lu started", (unsigned long)GetCurrentThreadId());
    // ---- step 2: DXVK's Vulkan objects ----
    IDirect3DDevice9 *const dev = render_outputs::device();
    MC2VR_LOG("xrprobe: device %p — QueryInterface(ID3D9VkInteropDevice)", (void *)dev);
    if (dev == nullptr) {
        MC2VR_LOG("xrprobe: no D3D9 device captured — stop");
        return 0;
    }
    ID3D9VkInteropDevice *interop = nullptr;
    if (FAILED(dev->QueryInterface(kIID_InteropDevice, reinterpret_cast<void **>(&interop))) ||
        interop == nullptr) {
        MC2VR_LOG("xrprobe: RESULT the device has no ID3D9VkInteropDevice (not DXVK?) — stop");
        return 0;
    }
    VkInstance vk_inst = nullptr;
    VkPhysicalDevice vk_phys = nullptr;
    VkDevice vk_dev = nullptr;
    VkQueue vk_queue = nullptr;
    uint32_t q_index = 0, q_family = 0;
    // Slots: 0-2 IUnknown, 3 GetVulkanHandles .. 8 ReleaseSubmissionQueue.
    if (!vtable_in_d3d9("ID3D9VkInteropDevice", interop, 9)) {
        MC2VR_LOG("xrprobe: RESULT the interop object's vtable leaves d3d9.dll — not calling it, stop");
        return 0;
    }
    MC2VR_LOG("xrprobe: interop %p — GetVulkanHandles", (void *)interop);
    interop->GetVulkanHandles(&vk_inst, &vk_phys, &vk_dev);
    MC2VR_LOG("xrprobe: GetSubmissionQueue");
    interop->GetSubmissionQueue(&vk_queue, &q_index, &q_family);
    MC2VR_LOG("xrprobe: DXVK Vulkan: instance %p physical %p device %p queue %p (family %u index %u)",
              (void *)vk_inst, (void *)vk_phys, (void *)vk_dev, (void *)vk_queue, q_family, q_index);

    const char *inst_ext[128] = {};
    UINT inst_ext_n = 128;
    IDirect3D9 *d3d = nullptr;
    ID3D9VkInteropInterface1 *iface1 = nullptr;
    if (SUCCEEDED(dev->GetDirect3D(&d3d)) && d3d != nullptr &&
        SUCCEEDED(d3d->QueryInterface(kIID_InteropInterface1, reinterpret_cast<void **>(&iface1))) &&
        iface1 != nullptr) {
        const HRESULT hr = iface1->GetInstanceExtensions(&inst_ext_n, inst_ext);
        MC2VR_LOG("xrprobe: DXVK instance extensions: %u%s", inst_ext_n,
                  hr == D3DERR_MOREDATA ? " (truncated)" : "");
    } else {
        inst_ext_n = 0;
        MC2VR_LOG("xrprobe: no ID3D9VkInteropInterface1 — instance extensions unchecked");
    }

    // ---- step 3: the runtime ----
    HMODULE xr = LoadLibraryW(L"wineopenxr.dll");
    auto negotiate = xr ? reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(
                              GetProcAddress(xr, "xrNegotiateLoaderRuntimeInterface"))
                        : nullptr;
    if (negotiate == nullptr) {
        MC2VR_LOG("xrprobe: RESULT no 32-bit wineopenxr.dll / negotiation export — stop");
        return 0;
    }
    XrNegotiateLoaderInfo li{};
    li.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
    li.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
    li.structSize = sizeof(li);
    li.minInterfaceVersion = 1;
    li.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    li.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
    li.maxApiVersion = XR_MAKE_VERSION(1, 0x3ff, 0xfff);
    XrNegotiateRuntimeRequest rr{};
    rr.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
    rr.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
    rr.structSize = sizeof(rr);
    const XrResult nr = negotiate(&li, &rr);
    MC2VR_LOG("xrprobe: negotiate -> %d (interface %u, api %u.%u.%u)", (int)nr, rr.runtimeInterfaceVersion,
              (unsigned)XR_VERSION_MAJOR(rr.runtimeApiVersion), (unsigned)XR_VERSION_MINOR(rr.runtimeApiVersion),
              (unsigned)XR_VERSION_PATCH(rr.runtimeApiVersion));
    if (XR_FAILED(nr) || rr.getInstanceProcAddr == nullptr) {
        MC2VR_LOG("xrprobe: RESULT runtime negotiation failed — stop");
        return 0;
    }
    g_gipa = rr.getInstanceProcAddr;

    uint32_t n = 0;
    auto enum_ext = proc<PFN_xrEnumerateInstanceExtensionProperties>("xrEnumerateInstanceExtensionProperties");
    bool has_vk = false;
    if (enum_ext != nullptr && XR_SUCCEEDED(enum_ext(nullptr, 0, &n, nullptr)) && n > 0) {
        static XrExtensionProperties props[256];
        n = n > 256 ? 256 : n;
        for (uint32_t i = 0; i < n; i++) props[i] = {XR_TYPE_EXTENSION_PROPERTIES};
        enum_ext(nullptr, n, &n, props);
        for (uint32_t i = 0; i < n; i++) {
            has_vk = has_vk || strcmp(props[i].extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0;
            MC2VR_LOG("xrprobe: runtime extension %s v%u", props[i].extensionName, props[i].extensionVersion);
        }
    }
    if (!has_vk) {
        MC2VR_LOG("xrprobe: RESULT runtime lacks XR_KHR_vulkan_enable — stop");
        return 0;
    }

    const char *exts[] = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ici.applicationInfo.applicationName, "mc2vr xr probe");
    strcpy(ici.applicationInfo.engineName, "mc2vr");
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = exts;
    if (!step("xrCreateInstance (32-bit, XR_KHR_vulkan_enable)",
              proc<PFN_xrCreateInstance>("xrCreateInstance")(&ici, &g_inst))) {
        return 0;
    }

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    if (step("xrGetSystem(HMD)", proc<PFN_xrGetSystem>("xrGetSystem")(g_inst, &sgi, &system))) {
        XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
        if (XR_SUCCEEDED(proc<PFN_xrGetSystemProperties>("xrGetSystemProperties")(g_inst, system, &sp))) {
            MC2VR_LOG("xrprobe: system '%s' maxSwapchain %ux%u", sp.systemName,
                      sp.graphicsProperties.maxSwapchainImageWidth,
                      sp.graphicsProperties.maxSwapchainImageHeight);
        }

        XrGraphicsRequirementsVulkanKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
        if (step("xrGetVulkanGraphicsRequirementsKHR",
                 proc<PFN_xrGetVulkanGraphicsRequirementsKHR>("xrGetVulkanGraphicsRequirementsKHR")(
                     g_inst, system, &req))) {
            MC2VR_LOG("xrprobe: Vulkan API min %u.%u max %u.%u",
                      (unsigned)XR_VERSION_MAJOR(req.minApiVersionSupported),
                      (unsigned)XR_VERSION_MINOR(req.minApiVersionSupported),
                      (unsigned)XR_VERSION_MAJOR(req.maxApiVersionSupported),
                      (unsigned)XR_VERSION_MINOR(req.maxApiVersionSupported));
        }
        static char list[8192];
        uint32_t len = 0;
        auto inst_req = proc<PFN_xrGetVulkanInstanceExtensionsKHR>("xrGetVulkanInstanceExtensionsKHR");
        if (inst_req != nullptr && XR_SUCCEEDED(inst_req(g_inst, system, sizeof(list), &len, list))) {
            check_extensions("instance", list, inst_ext_n ? inst_ext : nullptr, inst_ext_n);
        }
        auto dev_req = proc<PFN_xrGetVulkanDeviceExtensionsKHR>("xrGetVulkanDeviceExtensionsKHR");
        if (dev_req != nullptr && XR_SUCCEEDED(dev_req(g_inst, system, sizeof(list), &len, list))) {
            // DXVK's enabled device extensions aren't exposed by the interop;
            // DXVK requests these from wineopenxr itself at device creation.
            check_extensions("device", list, nullptr, 0);
        }
        VkPhysicalDevice want = nullptr;
        if (step("xrGetVulkanGraphicsDeviceKHR(DXVK instance)",
                 proc<PFN_xrGetVulkanGraphicsDeviceKHR>("xrGetVulkanGraphicsDeviceKHR")(
                     g_inst, system, vk_inst, &want))) {
            MC2VR_LOG("xrprobe: runtime wants physical device %p, DXVK uses %p — %s", (void *)want,
                      (void *)vk_phys, want == vk_phys ? "MATCH" : "MISMATCH");
        }

        // ---- step 4: a session on DXVK's device ----
        run_session(interop, vk_inst, vk_phys, vk_dev, vk_queue, q_index, q_family, system);
    }

    step("xrDestroyInstance", proc<PFN_xrDestroyInstance>("xrDestroyInstance")(g_inst));
    g_inst = XR_NULL_HANDLE;
    MC2VR_LOG("xrprobe: done");
    return 0;
}

} // namespace

bool set_enabled(const char *value)
{
    if (strcmp(value, "on") == 0) g_enabled = true;
    else if (strcmp(value, "off") == 0) g_enabled = false;
    else return false;
    MC2VR_LOG("xrprobe: debug_xr_probe=%s", value);
    return true;
}

void install_early()
{
    if (!g_enabled) {
        return;
    }
    // The fault log only: the Wine VR registry (negotiation state + the
    // runtime's Vulkan requirements DXVK reads) is published by the launcher
    // before the game starts (src/launcher/vr_registry.h).
    AddVectoredExceptionHandler(1, fault_log);
}

void start()
{
    if (!g_enabled || g_started) {
        return;
    }
    g_started = true;
    if (ipc::connected()) {
        MC2VR_LOG("xrprobe: a host is connected — not probing (two sessions would conflict; "
                  "run with MC2VR_NO_HOST=1)");
        return;
    }
    MC2VR_LOG("xrprobe: starting the probe thread");
    HANDLE t = CreateThread(nullptr, 0, probe_thread, nullptr, 0, nullptr);
    if (t == nullptr) {
        MC2VR_LOG("xrprobe: CreateThread failed");
        return;
    }
    CloseHandle(t);
}

} // namespace mc2vr::xr_probe
