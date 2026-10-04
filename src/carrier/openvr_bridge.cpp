// S4-1: OpenVR bridge. See openvr_bridge.hpp for the mission. Layout facts in
// this file come from Valve's openvr.h (SDK 2.15, IVRSystem_026) — the vtable
// order of the FnTable fields must match the interface version requested;
// VR_IsInterfaceVersionValid is checked first and the FnTable is only used if
// the runtime serves exactly IVRSystem_026.
//
// MinGW g++ cannot call MSVC thiscall vtables on i386, so this module uses the
// official `FnTable:` C bindings: VR_GetGenericInterface("FnTable:IVRSystem_026")
// returns a struct of plain __cdecl function pointers taking the interface
// object as the first argument. Stage 1 deliberately calls only functions with
// no struct-by-value returns (i386 sret ABI left untested for now).

#include "openvr_bridge.hpp"

#include <windows.h>

#include "log.hpp"

#include <cstdint>
#include <cstring>

namespace mc2vr::ovr {

// ---------------------------------------------------------------------------
// Minimal OpenVR POD types (openvr.h, pack(8) — no 8-byte members here, so the
// i386 default packing produces the identical layout).
// ---------------------------------------------------------------------------

enum EVRInitError {
    VRInitError_None = 0,
};
enum EVRApplicationType {
    VRApplication_Scene = 1,
};
enum EVREye { Eye_Left = 0, Eye_Right = 1 };
enum ETrackingUniverseOrigin { TrackingUniverseStanding = 1 };
enum ETrackedDeviceClass {
    TrackedDeviceClass_Invalid = 0,
    TrackedDeviceClass_HMD = 1,
};
enum ETrackedPropertyError { TrackedProp_Success = 0 };
enum ETrackingResult { TrackingResult_Running_OK = 200 };

// General/HMD string+float properties (openvr.h ETrackedDeviceProperty).
enum ETrackedDeviceProperty {
    Prop_ModelNumber_String = 1001,
    Prop_SerialNumber_String = 1002,
    Prop_ManufacturerName_String = 1005,
    Prop_DisplayFrequency_Float = 2002,
    Prop_UserIpdMeters_Float = 2003,
};

struct HmdMatrix34_t {
    float m[3][4];
};
struct HmdVector3_t {
    float v[3];
};
struct TrackedDevicePose_t {
    HmdMatrix34_t mDeviceToAbsoluteTracking;
    HmdVector3_t vVelocity;
    HmdVector3_t vAngularVelocity;
    ETrackingResult eTrackingResult;
    bool bPoseIsValid;
    bool bDeviceIsConnected;
};

// ---------------------------------------------------------------------------
// DLL exports (openvr_api_dxvk.dll, extern "C" __cdecl).
// ---------------------------------------------------------------------------

typedef bool(__cdecl *VR_IsHmdPresent_t)();
typedef bool(__cdecl *VR_IsRuntimeInstalled_t)();
typedef bool(__cdecl *VR_IsInterfaceVersionValid_t)(const char *version);
typedef uint32_t(__cdecl *VR_InitInternal2_t)(int32_t *error,
                                              int32_t app_type,
                                              const char *startup_info);
typedef void(__cdecl *VR_ShutdownInternal_t)();
typedef void *(__cdecl *VR_GetGenericInterface_t)(const char *version,
                                                  int32_t *error);
typedef const char *(__cdecl *VR_GetVRInitErrorAsEnglishDescription_t)(
    int32_t error);

// ---------------------------------------------------------------------------
// IVRSystem_022 FnTable — fields in exact openvr.h (SDK 1.16.8) virtual-method
// order. Layout source: vendor/openvr/openvr_1.16.8.h — PINNED, not master:// the 20261001 Proton vrclient's served IVRSystem_026 FnTable empirically does
// NOT match the SDK 2.15 public header (2026-10-04: slot-28 string-property call
// crashed the probe), while the pinned 1.16.8 IVRCompositor_022 layout worked
// 1:1 under the same runtime. The runtime serves 022 on every client observed.
// Unused slots are kept as void* so the used slots sit at the right indices;
// the static_asserts below lock the ones we rely on.

struct VRSystem_FnTable_022 {
    void (*GetRecommendedRenderTargetSize)(void *self, uint32_t *w, uint32_t *h); // 0
    void *GetProjectionMatrix;                                                    // 1
    void (*GetProjectionRaw)(void *self, EVREye eye, float *left, float *right,
                             float *top, float *bottom);                          // 2
    void *ComputeDistortion;                                                      // 3
    void *GetEyeToHeadTransform;                                                  // 4 (struct return — S4-3)
    void *GetTimeSinceLastVsync;                                                 // 5
    int32_t (*GetD3D9AdapterIndex)(void *self);                                   // 6
    void *GetDXGIOutputInfo;                                                     // 7
    void *GetOutputDevice;                                                        // 8
    void *IsDisplayOnDesktop;                                                     // 9
    void *SetDisplayVisibility;                                                   // 10
    void (*GetDeviceToAbsoluteTrackingPose)(void *self,
                                            ETrackingUniverseOrigin origin,
                                            float predicted_seconds,
                                            TrackedDevicePose_t *poses,
                                            uint32_t count);                      // 11
    void *GetSeatedZeroPoseToStandingAbsoluteTrackingPose;                       // 12
    void *GetRawZeroPoseToStandingAbsoluteTrackingPose;                           // 13
    void *GetSortedTrackedDeviceIndicesOfClass;                                   // 14
    void *GetTrackedDeviceActivityLevel;                                         // 15
    void *ApplyTransform;                                                         // 16
    void *GetTrackedDeviceIndexForControllerRole;                                 // 17
    void *GetControllerRoleForTrackedDeviceIndex;                                 // 18
    int32_t (*GetTrackedDeviceClass)(void *self, uint32_t index);                 // 19
    void *IsTrackedDeviceConnected;                                              // 20
    void *GetBoolTrackedDeviceProperty;                                          // 21
    float (*GetFloatTrackedDeviceProperty)(void *self, uint32_t index,
                                           int32_t prop, int32_t *error);         // 22
    int32_t (*GetInt32TrackedDeviceProperty)(void *self, uint32_t index,
                                             int32_t prop, int32_t *error);       // 23
    void *GetUint64TrackedDeviceProperty;                                        // 24
    void *GetMatrix34TrackedDeviceProperty;                                      // 25
    void *GetArrayTrackedDeviceProperty;                                          // 26
    uint32_t (*GetStringTrackedDeviceProperty)(void *self, uint32_t index,
                                               int32_t prop, char *value,
                                               uint32_t size,
                                               int32_t *error);                   // 27
};

static_assert(__builtin_offsetof(VRSystem_FnTable_022, GetProjectionRaw) ==
              2 * sizeof(void *), "IVRSystem_022 slot 2 (GetProjectionRaw)");
static_assert(__builtin_offsetof(VRSystem_FnTable_022, GetD3D9AdapterIndex) ==
              6 * sizeof(void *), "IVRSystem_022 slot 6 (GetD3D9AdapterIndex)");
static_assert(__builtin_offsetof(VRSystem_FnTable_022,
                                 GetDeviceToAbsoluteTrackingPose) ==
              11 * sizeof(void *),
              "IVRSystem_022 slot 11 (GetDeviceToAbsoluteTrackingPose)");
static_assert(__builtin_offsetof(VRSystem_FnTable_022,
                                 GetStringTrackedDeviceProperty) ==
              27 * sizeof(void *),
              "IVRSystem_022 slot 27 (GetStringTrackedDeviceProperty)");

// FnTable version strings we request; the layout above matches exactly _022.
static const char *kIVRSystem_Version = "IVRSystem_022";

// Probe list — logged so a mismatch tells us which header to pull next.
static const char *kProbeVersions[] = {
    "IVRSystem_026", "IVRSystem_025", "IVRSystem_024", "IVRSystem_023",
    "IVRSystem_022", "IVRCompositor_029", "IVRCompositor_028",
    "IVRCompositor_027", "IVRCompositor_026", "IVRCompositor_022",
};

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static bool g_enabled = false;
static bool g_ready = false;
static HMODULE g_vr_dll = nullptr;

static VR_IsHmdPresent_t p_VR_IsHmdPresent;
static VR_IsRuntimeInstalled_t p_VR_IsRuntimeInstalled;
static VR_IsInterfaceVersionValid_t p_VR_IsInterfaceVersionValid;
static VR_InitInternal2_t p_VR_InitInternal2;
static VR_ShutdownInternal_t p_VR_ShutdownInternal;
static VR_GetGenericInterface_t p_VR_GetGenericInterface;
static VR_GetVRInitErrorAsEnglishDescription_t
    p_VR_GetVRInitErrorAsEnglishDescription;

static VRSystem_FnTable_022 *g_system = nullptr;

bool set_enabled(const char *value)
{
    if (strcmp(value, "on") == 0) {
        g_enabled = true;
    } else if (strcmp(value, "off") == 0) {
        g_enabled = false;
    } else {
        return false;
    }
    return true;
}

bool ready()
{
    return g_ready;
}

// ---------------------------------------------------------------------------
// Bootstrap
// ---------------------------------------------------------------------------

static bool load_bridge_dll()
{
    // The 32-bit system dir (syswow64 in this prefix) is on the default search
    // path of this 32-bit process, so the plain name finds the right DLL.
    g_vr_dll = LoadLibraryW(L"openvr_api_dxvk.dll");
    if (g_vr_dll == nullptr) {
        wchar_t dir[MAX_PATH];
        UINT n = GetSystemDirectoryW(dir, MAX_PATH);
        if (n > 0 && n + 32 < MAX_PATH) {
            wcscat(dir, L"\\openvr_api_dxvk.dll");
            g_vr_dll = LoadLibraryW(dir);
        }
    }
    if (g_vr_dll == nullptr) {
        MC2VR_LOG("ovr: openvr_api_dxvk.dll not found (GetLastError=%lu) — "
                  "is SteamVR-for-Proton installed in this prefix?",
                  GetLastError());
        return false;
    }

#define RESOLVE(name)                                                        \
    do {                                                                     \
        p_##name = reinterpret_cast<decltype(p_##name)>(                    \
            reinterpret_cast<void *>(GetProcAddress(g_vr_dll, #name)));      \
        if (p_##name == nullptr) {                                           \
            MC2VR_LOG("ovr: openvr_api_dxvk.dll missing export %s", #name);  \
            return false;                                                    \
        }                                                                    \
    } while (0)

    RESOLVE(VR_IsHmdPresent);
    RESOLVE(VR_IsRuntimeInstalled);
    RESOLVE(VR_IsInterfaceVersionValid);
    RESOLVE(VR_InitInternal2);
    RESOLVE(VR_ShutdownInternal);
    RESOLVE(VR_GetGenericInterface);
    RESOLVE(VR_GetVRInitErrorAsEnglishDescription);
#undef RESOLVE

    MC2VR_LOG("ovr: openvr_api_dxvk.dll loaded, exports resolved");
    return true;
}

static void log_version_probes()
{
    for (const char *v : kProbeVersions) {
        MC2VR_LOG("ovr: version %s valid=%d", v,
                  p_VR_IsInterfaceVersionValid(v) ? 1 : 0);
    }
}

// One HMD pose sample — pre-tests the S4-3 feed path and logs live tracking.
static void log_pose_sample()
{
    TrackedDevicePose_t pose;
    memset(&pose, 0, sizeof(pose));
    g_system->GetDeviceToAbsoluteTrackingPose(g_system, TrackingUniverseStanding,
                                              0.0f, &pose, 1);
    MC2VR_LOG("ovr: pose sample: valid=%d connected=%d result=%d",
              pose.bPoseIsValid ? 1 : 0, pose.bDeviceIsConnected ? 1 : 0,
              (int)pose.eTrackingResult);
    MC2VR_LOG("ovr: pose m = [%.3f %.3f %.3f %.3f; %.3f %.3f %.3f %.3f; "
              "%.3f %.3f %.3f %.3f]",
              pose.mDeviceToAbsoluteTracking.m[0][0],
              pose.mDeviceToAbsoluteTracking.m[0][1],
              pose.mDeviceToAbsoluteTracking.m[0][2],
              pose.mDeviceToAbsoluteTracking.m[0][3],
              pose.mDeviceToAbsoluteTracking.m[1][0],
              pose.mDeviceToAbsoluteTracking.m[1][1],
              pose.mDeviceToAbsoluteTracking.m[1][2],
              pose.mDeviceToAbsoluteTracking.m[1][3],
              pose.mDeviceToAbsoluteTracking.m[2][0],
              pose.mDeviceToAbsoluteTracking.m[2][1],
              pose.mDeviceToAbsoluteTracking.m[2][2],
              pose.mDeviceToAbsoluteTracking.m[2][3]);
}

static void bootstrap()
{
    MC2VR_LOG("ovr: OpenVR bootstrap starting (tid=%lu)",
              GetCurrentThreadId());

    if (!load_bridge_dll()) {
        MC2VR_LOG("ovr: bootstrap aborted — bridge DLL unavailable");
        return;
    }

    MC2VR_LOG("ovr: runtime installed=%d hmd present=%d",
              p_VR_IsRuntimeInstalled() ? 1 : 0, p_VR_IsHmdPresent() ? 1 : 0);

    // Prefix-VR setup (2026-10-04 root cause of err 105): the current Proton
    // vrclient hard-requires the VR Vulkan instance-extension cache that
    // vrclient_init_registry populates IN-PROCESS (it is not persisted to the
    // registry). On Steam launches some setup path invokes it during VR
    // prefix preparation; our direct `proton run` chain never does — without
    // it, vrclient fails at load_vrclient with "Could not create key, status
    // 0x2" and VR_InitInternal2 returns 105 before any IPC. Calling it here
    // made the standalone probe connect instantly (proven, twice).
    {
        HMODULE vrc = LoadLibraryW(L"vrclient.dll");
        if (vrc == nullptr) {
            MC2VR_LOG("ovr: vrclient.dll load failed (%lu) — init may fail",
                      GetLastError());
        } else {
            auto p_initreg = reinterpret_cast<int(__cdecl *)(void *)>(
                reinterpret_cast<void *>(GetProcAddress(vrc,
                                                        "vrclient_init_registry")));
            if (p_initreg == nullptr) {
                MC2VR_LOG("ovr: vrclient_init_registry export missing");
            } else {
                // Params struct layout is opaque; a generously zeroed buffer
                // worked 1:1 in the probe.
                static unsigned char params[256];
                const int rc = p_initreg(params);
                MC2VR_LOG("ovr: vrclient_init_registry -> %d", rc);
            }
        }
    }

    // Connect FIRST (2026-10-04 live lesson): VR_IsHmdPresent and
    // VR_IsInterfaceVersionValid query the connected vrserver — before
    // VR_InitInternal2 there is no connection and they all report 0 no matter
    // what SteamVR serves. Init also asks Steam to start SteamVR if it isn't
    // up, so retry briefly to cover the startup race.
    int32_t err = VRInitError_None;
    uint32_t token = 0;
    for (int attempt = 1;; attempt++) {
        err = VRInitError_None;
        token = p_VR_InitInternal2(&err, VRApplication_Scene, "mc2vr");
        if (err == VRInitError_None) {
            break;
        }
        MC2VR_LOG("ovr: VR_InitInternal2 attempt %d failed err=%d (%s)", attempt,
                  err, p_VR_GetVRInitErrorAsEnglishDescription(err));
        if (attempt >= 3) {
            MC2VR_LOG("ovr: giving up on SteamVR — bridge stays idle");
            return;
        }
        Sleep(5000);
    }
    MC2VR_LOG("ovr: VR_InitInternal2 ok (token=%u) — SteamVR connected", token);

    // Post-init probes: NOW these reflect what the runtime actually serves.
    log_version_probes();

    // Only use the _022 FnTable when the runtime serves exactly that version —
    // the struct layout above is version-specific (see the header comment).
    if (!p_VR_IsInterfaceVersionValid(kIVRSystem_Version)) {
        MC2VR_LOG("ovr: runtime does not serve %s — bridge stays idle; pull "
                  "the matching openvr.h and adjust the FnTable",
                  kIVRSystem_Version);
        return;
    }

    int32_t iface_err = VRInitError_None;
    void *iface = p_VR_GetGenericInterface("FnTable:IVRSystem_022", &iface_err);
    if (iface == nullptr || iface_err != VRInitError_None) {
        MC2VR_LOG("ovr: FnTable:IVRSystem_026 unavailable err=%d (%s)",
                  iface_err,
                  p_VR_GetVRInitErrorAsEnglishDescription(iface_err));
        return;
    }
    g_system = static_cast<VRSystem_FnTable_022 *>(iface);
    MC2VR_LOG("ovr: IVRSystem_022 FnTable acquired");

    // ---- Smoke-test facts (S4-2/3 inputs) ----

    if (g_system->GetTrackedDeviceClass(g_system, 0) != TrackedDeviceClass_HMD) {
        MC2VR_LOG("ovr: device 0 is not the HMD (unexpected)");
    }

    char str[256];
    int32_t prop_err = TrackedProp_Success;
    memset(str, 0, sizeof(str));
    g_system->GetStringTrackedDeviceProperty(g_system, 0,
                                             Prop_ManufacturerName_String, str,
                                             sizeof(str), &prop_err);
    MC2VR_LOG("ovr: HMD manufacturer = \"%s\" (err=%d)", str, prop_err);
    memset(str, 0, sizeof(str));
    g_system->GetStringTrackedDeviceProperty(g_system, 0,
                                             Prop_ModelNumber_String, str,
                                             sizeof(str), &prop_err);
    MC2VR_LOG("ovr: HMD model = \"%s\" (err=%d)", str, prop_err);
    memset(str, 0, sizeof(str));
    g_system->GetStringTrackedDeviceProperty(g_system, 0,
                                             Prop_SerialNumber_String, str,
                                             sizeof(str), &prop_err);
    MC2VR_LOG("ovr: HMD serial = \"%s\" (err=%d)", str, prop_err);

    uint32_t w = 0, h = 0;
    g_system->GetRecommendedRenderTargetSize(g_system, &w, &h);
    MC2VR_LOG("ovr: recommended render target = %ux%u", w, h);

    MC2VR_LOG("ovr: D3D9 adapter index = %d",
              g_system->GetD3D9AdapterIndex(g_system));

    float ipd = g_system->GetFloatTrackedDeviceProperty(
        g_system, 0, Prop_UserIpdMeters_Float, &prop_err);
    float freq = g_system->GetFloatTrackedDeviceProperty(
        g_system, 0, Prop_DisplayFrequency_Float, &prop_err);
    MC2VR_LOG("ovr: HMD IPD = %.4f m, display freq = %.1f Hz", ipd, freq);

    float pl, pr, pt, pb;
    g_system->GetProjectionRaw(g_system, Eye_Left, &pl, &pr, &pt, &pb);
    MC2VR_LOG("ovr: proj raw L = [l%.4f r%.4f t%.4f b%.4f]", pl, pr, pt, pb);
    g_system->GetProjectionRaw(g_system, Eye_Right, &pl, &pr, &pt, &pb);
    MC2VR_LOG("ovr: proj raw R = [l%.4f r%.4f t%.4f b%.4f]", pl, pr, pt, pb);

    log_pose_sample();

    g_ready = true;
    MC2VR_LOG("ovr: OpenVR bootstrap COMPLETE — SteamVR live, S4-2 can submit");

    // vrclient_init_registry PERSISTS HKLM\Software\Wine\VR (live-proven
    // 2026-10-04: it wrote openvr_vulkan_instance_extensions="" into
    // system.reg). That value makes DXVK's d3d9 enable its boot-time OpenVR
    // interop on the NEXT game start — which hung the game at a white screen
    // pre-boot (live-observed; DXVK's Scene init blocks in device creation
    // while SteamVR is mid-transition). Until S4-2 proves the DXVK interop
    // path is wanted AND safe at boot, scrub the key so game boots stay
    // clean. Both writer and this cleanup are 32-bit, same registry view.
    if (RegDeleteTreeA(HKEY_LOCAL_MACHINE, "Software\\Wine\\VR") ==
        ERROR_SUCCESS) {
        MC2VR_LOG("ovr: scrubbed persisted Software\\Wine\\VR (DXVK boot "
                  "interop stays off)");
    }
}

static DWORD WINAPI bootstrap_thread(void *)
{
    bootstrap();
    return 0;
}

void init()
{
    if (!g_enabled) {
        return;
    }
    // Dedicated thread (2026-10-04): early OpenVR calls (VR_IsHmdPresent,
    // vrclient_init_registry's compositor connect, VR_InitInternal2's Scene
    // handshake) can BLOCK for long stretches while vrserver is mid-transition
    // (live-observed: HMD standby cycling stalled the probe for a minute). The
    // game must never wait on SteamVR — hook installation and gameplay proceed
    // regardless; S4-2/3 gate on ovr::ready(). Thread leaks by design like all
    // carrier objects.
    HANDLE t = CreateThread(nullptr, 0, bootstrap_thread, nullptr, 0, nullptr);
    if (t != nullptr) {
        CloseHandle(t);
        MC2VR_LOG("ovr: bootstrap spawned on its own thread");
    } else {
        MC2VR_LOG("ovr: CreateThread failed (%lu) — running bootstrap "
                  "synchronously", GetLastError());
        bootstrap();
    }
}

} // namespace mc2vr::ovr
