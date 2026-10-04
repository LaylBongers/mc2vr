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

// ABI NOTE (2026-10-04 live-proven): the FnTable entries Proton serves are
// THISCALL thunks that load the IVRSystem object into ECX THEMSELVES (built
// in create_winIVRSystem_IVRSystem_022_FnTable: "mov ecx, obj; mov edx,
// wrapper; jmp edx"). The first STACK argument is the method's first
// parameter — NOT self. Passing an explicit self shifts every argument one
// slot: run 5's "device 0 is not the HMD" was the class query reading the
// table pointer as the device index, the string query then fed a NULL
// buffer pointer into the server, and the probe's RT-size query printed the
// real HEIGHT in *w (2240) with *h never written (0) while the real width
// was scribbled over the table object. Call these WITHOUT self.
struct VRSystem_FnTable_022 {
    void (*GetRecommendedRenderTargetSize)(uint32_t *w, uint32_t *h);             // 0
    void *GetProjectionMatrix;                                                    // 1
    void (*GetProjectionRaw)(EVREye eye, float *left, float *right,
                             float *top, float *bottom);                          // 2
    void *ComputeDistortion;                                                      // 3
    void *GetEyeToHeadTransform;                                                  // 4 (struct return — S4-3)
    void *GetTimeSinceLastVsync;                                                  // 5
    int32_t (*GetD3D9AdapterIndex)();                                             // 6
    void *GetDXGIOutputInfo;                                                     // 7
    void *GetOutputDevice;                                                        // 8
    void *IsDisplayOnDesktop;                                                     // 9
    void *SetDisplayVisibility;                                                   // 10
    void (*GetDeviceToAbsoluteTrackingPose)(ETrackingUniverseOrigin origin,
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
    int32_t (*GetTrackedDeviceClass)(uint32_t index);                              // 19
    void *IsTrackedDeviceConnected;                                              // 20
    void *GetBoolTrackedDeviceProperty;                                          // 21
    float (*GetFloatTrackedDeviceProperty)(uint32_t index,
                                           int32_t prop, int32_t *error);         // 22
    int32_t (*GetInt32TrackedDeviceProperty)(uint32_t index,
                                             int32_t prop, int32_t *error);       // 23
    void *GetUint64TrackedDeviceProperty;                                        // 24
    void *GetMatrix34TrackedDeviceProperty;                                      // 25
    void *GetArrayTrackedDeviceProperty;                                          // 26
    uint32_t (*GetStringTrackedDeviceProperty)(uint32_t index,
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
static bool g_init_registry = false;
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

// The err-105 fix (RE-proven in load_vrclient): vrclient requires
// HKCU\Software\Wine\VR with PROTON_VR_RUNTIME, state (DWORD, nonzero =
// ready; 0 makes the reader BLOCK on a registry-change notification) and
// openvr_vulkan_instance_extensions, else it unloads with "Could not create
// key, status 0x2" and VR_InitInternal2 returns 105.
//
// WHY THE CARRIER WRITES THESE ITSELF (run 6a, live-proven 2026-10-04):
// the same registry key arms DXVK's d3d9 BOOT-TIME OpenVR interop — with
// the values present at process start, the game hung before the main loop
// (boot marker never ticked; SteamVR was mid-flap at the time, exactly the
// white-screen landmine). DXVK reads the key at DEVICE CREATION (game boot,
// before we exist); vrclient reads it at VR_InitInternal2 time (post-attach).
// Writing here — after the device exists, before our init — serves vrclient
// while keeping every game boot interop-free and SteamVR-state-independent.
// S4-2 implication: DXVK's boot-time interop stays OFF by design; the S4-2
// texture-sharing decision must either use a runtime-interop path or
// deliberately re-arm the boot value with a SteamVR-stability gate.
static bool ensure_vr_registry()
{
    HKEY key = nullptr;
    DWORD disp = 0;
    LSTATUS rc = RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\Wine\\VR", 0,
                                nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                                &disp);
    if (rc != ERROR_SUCCESS) {
        MC2VR_LOG("ovr: RegCreateKeyExA(Software\\Wine\\VR) failed %lu",
                  rc);
        return false;
    }

    char rt[MAX_PATH];
    const DWORD n = GetEnvironmentVariableA("PROTON_VR_RUNTIME", rt,
                                            sizeof(rt));
    if (n == 0 || n >= sizeof(rt)) {
        RegCloseKey(key);
        MC2VR_LOG("ovr: PROTON_VR_RUNTIME env missing (proton run always "
                  "sets it) — cannot write the runtime path");
        return false;
    }

    bool ok = true;
    if (RegSetValueExA(key, "PROTON_VR_RUNTIME", 0, REG_SZ,
                       reinterpret_cast<const BYTE *>(rt), n) !=
        ERROR_SUCCESS) {
        ok = false;
    }
    const DWORD state = 1; // ready — probe-proven; see the comment above
    if (RegSetValueExA(key, "state", 0, REG_DWORD,
                       reinterpret_cast<const BYTE *>(&state),
                       sizeof(state)) != ERROR_SUCCESS) {
        ok = false;
    }
    // The compositor reports EMPTY required instance extensions — "" is the
    // semantically correct value (probe-verified).
    if (RegSetValueExA(key, "openvr_vulkan_instance_extensions", 0, REG_SZ,
                       reinterpret_cast<const BYTE *>(""), 1) !=
        ERROR_SUCCESS) {
        ok = false;
    }
    RegCloseKey(key);
    MC2VR_LOG("ovr: VR registry values written (disp=%lu, %s)",
              disp, ok ? "ok" : "PARTIAL — some writes failed");
    return ok;
}

// Remove the key again once vrclient has loaded (the values are only read at
// load time, guarded by _vrclient_loaded). Without this the values persist
// and arm DXVK's boot-time interop for the NEXT game start (run 6a).
static void cleanup_vr_registry()
{
    if (RegDeleteKeyA(HKEY_CURRENT_USER, "Software\\Wine\\VR") ==
        ERROR_SUCCESS) {
        MC2VR_LOG("ovr: VR registry values removed (next boot stays "
                  "interop-free)");
    }
}

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

bool set_init_registry(const char *value)
{
    if (strcmp(value, "on") == 0) {
        g_init_registry = true;
    } else if (strcmp(value, "off") == 0) {
        g_init_registry = false;
    } else {
        return false;
    }
    MC2VR_LOG("ovr: openvr_init_registry=%s", value);
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
    g_system->GetDeviceToAbsoluteTrackingPose(TrackingUniverseStanding,
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

    // err-105 fix, carrier-side (see ensure_vr_registry): must run BEFORE
    // VR_InitInternal2 — vrclient reads these values at init2 time. Writing
    // them AFTER the game's device creation keeps DXVK's boot-time interop
    // disarmed (run 6a live-proven: the value present at boot hung the game
    // pre-main-loop with SteamVR mid-transition).
    if (!ensure_vr_registry()) {
        MC2VR_LOG("ovr: registry ensure failed — VR_InitInternal2 will "
                  "likely err 105; continuing (bootstrap retries fail-soft)");
    }

    // Prefix-VR setup (2026-10-04 root cause of err 105): the current Proton
    // vrclient hard-requires the VR Vulkan instance-extension cache that
    // vrclient_init_registry populates IN-PROCESS (it is not persisted to the
    // registry). On Steam launches some setup path invokes it during VR
    // prefix preparation; our direct `proton run` chain never does — without
    // it, vrclient fails at load_vrclient with "Could not create key, status
    // 0x2" and VR_InitInternal2 returns 105 before any IPC. Calling it here
    // made the standalone probe connect instantly (proven, twice).
    //
    // DEFAULT OFF since run 5 (2026-10-04): in-game this call is a
    // LIVE-PROVEN DEADLOCK. vrclient_init_registry starts a Background-type
    // vrclient session (compositor connect + Vulkan extension enumeration —
    // the VK_EXT_debug_utils warnings in vrclient_MERCENARIES2.txt), returns,
    // and its internal VR_Shutdown fires ~200 ms LATER — after our own
    // VR_InitInternal2 — tearing down the shared vrclient state under our
    // FnTable (vrserver logs "Socket closed"/"disconnected" for this pid).
    // The version probes kept answering from cached state, then
    // GetTrackedDeviceClass returned garbage and GetStringTrackedDeviceProperty
    // BLOCKED FOREVER; the render thread froze within 5 ms of that call —
    // the Vulkan/compositor work couples to DXVK, which the render thread
    // was stuck in. Complete game hang, first in-game Stage-1 attempt.
    // The standalone probe never reproduced this: without a live DXVK
    // device the compositor-connect path doesn't run, so no Background
    // session and no teardown race. Re-enable ONLY as a deliberate
    // experiment with a kill plan.
    if (!g_init_registry) {
        MC2VR_LOG("ovr: vrclient_init_registry SKIPPED "
                  "(openvr_init_registry=off; in-game it deadlocks — see "
                  "docs/s4_handover.md run 5; without it init may err 105)");
    } else {
        MC2VR_LOG("ovr: WARNING — calling vrclient_init_registry in-game "
                  "(openvr_init_registry=on) — live-proven deadlock risk");
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
            cleanup_vr_registry();
            return;
        }
        Sleep(5000);
    }
    MC2VR_LOG("ovr: VR_InitInternal2 ok (token=%u) — SteamVR connected", token);

    // vrclient has now loaded (or failed permanently) — the registry values
    // served their purpose. Remove them so the NEXT game boot finds the key
    // absent (DXVK boot-time interop stays disarmed; vrclient does not
    // re-read them, _vrclient_loaded guards the load).
    cleanup_vr_registry();

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
    // Run-5 lesson: after a session teardown, cached calls keep answering but
    // the first real IPC call BLOCKED FOREVER and took the render thread down
    // with it. So: (1) device 0 must be the HMD before anything else — a
    // wrong class means the session is suspect (torn down, or no HMD) and we
    // ABORT instead of calling deeper; (2) every remaining call is preceded
    // by an enter-log so any future hang pinpoints the exact call.
    // Call entries WITHOUT self (see the ABI note at the struct).
    const int32_t dev_class = g_system->GetTrackedDeviceClass(0);
    MC2VR_LOG("ovr: device 0 class = %d (want 1 = HMD)", dev_class);
    if (dev_class != TrackedDeviceClass_HMD) {
        MC2VR_LOG("ovr: device 0 is not the HMD — session suspect; aborting "
                  "smoke test, bridge stays not-ready (fail-soft)");
        return;
    }

    char str[256];
    int32_t prop_err = TrackedProp_Success;
    memset(str, 0, sizeof(str));
    MC2VR_LOG("ovr: querying manufacturer ...");
    g_system->GetStringTrackedDeviceProperty(0,
                                              Prop_ManufacturerName_String, str,
                                              sizeof(str), &prop_err);
    MC2VR_LOG("ovr: HMD manufacturer = \"%s\" (err=%d)", str, prop_err);
    memset(str, 0, sizeof(str));
    MC2VR_LOG("ovr: querying model ...");
    g_system->GetStringTrackedDeviceProperty(0,
                                             Prop_ModelNumber_String, str,
                                             sizeof(str), &prop_err);
    MC2VR_LOG("ovr: HMD model = \"%s\" (err=%d)", str, prop_err);
    memset(str, 0, sizeof(str));
    MC2VR_LOG("ovr: querying serial ...");
    g_system->GetStringTrackedDeviceProperty(0,
                                             Prop_SerialNumber_String, str,
                                             sizeof(str), &prop_err);
    MC2VR_LOG("ovr: HMD serial = \"%s\" (err=%d)", str, prop_err);

    uint32_t w = 0, h = 0;
    MC2VR_LOG("ovr: querying recommended target size ...");
    g_system->GetRecommendedRenderTargetSize(&w, &h);
    MC2VR_LOG("ovr: recommended render target = %ux%u", w, h);

    MC2VR_LOG("ovr: querying D3D9 adapter index ...");
    MC2VR_LOG("ovr: D3D9 adapter index = %d", g_system->GetD3D9AdapterIndex());

    MC2VR_LOG("ovr: querying IPD + display frequency ...");
    float ipd = g_system->GetFloatTrackedDeviceProperty(
        0, Prop_UserIpdMeters_Float, &prop_err);
    float freq = g_system->GetFloatTrackedDeviceProperty(
        0, Prop_DisplayFrequency_Float, &prop_err);
    MC2VR_LOG("ovr: HMD IPD = %.4f m, display freq = %.1f Hz", ipd, freq);

    float pl, pr, pt, pb;
    MC2VR_LOG("ovr: querying raw projection L ...");
    g_system->GetProjectionRaw(Eye_Left, &pl, &pr, &pt, &pb);
    MC2VR_LOG("ovr: proj raw L = [l%.4f r%.4f t%.4f b%.4f]", pl, pr, pt, pb);
    MC2VR_LOG("ovr: querying raw projection R ...");
    g_system->GetProjectionRaw(Eye_Right, &pl, &pr, &pt, &pb);
    MC2VR_LOG("ovr: proj raw R = [l%.4f r%.4f t%.4f b%.4f]", pl, pr, pt, pb);

    MC2VR_LOG("ovr: sampling pose ...");
    log_pose_sample();

    g_ready = true;
    MC2VR_LOG("ovr: OpenVR bootstrap COMPLETE — SteamVR live, S4-2 can submit");

    // NOTE (2026-10-04): the old post-bootstrap registry scrub is REMOVED — it
    // targeted HKLM (wrong hive; the real key is HKCU\Software\Wine\VR) and
    // those values are now REQUIRED by vrclient at load (launch.sh asserts
    // them; deleting them re-breaks err 105). The DXVK boot-interop landmine
    // is handled at the design level instead (docs/s4_handover.md run 6).
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
