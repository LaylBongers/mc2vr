// Standalone OpenVR connectivity probe (S4-1 diagnostics — NOT part of the
// carrier). Replicates the carrier's openvr_api_dxvk.dll init sequence outside
// the game so failures can be attributed without a live game run:
//   - logs which module paths actually load (openvr_api_dxvk, vrclient)
//   - tries VR_InitInternal2(Scene), then legacy VR_InitInternal(Scene),
//     then VR_InitInternal2(Background) — the last one isolates
//     compositor/HMD-side init from server connectivity
//   - on success: version probes + IVRSystem_026 FnTable smoke facts
//
// Build: i686-w64-mingw32-g++ -static -mconsole -o openvr_probe.exe openvr_probe.cpp
// Run:   <proton>/proton run openvr_probe.exe   (same env as launch.sh)
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <tlhelp32.h>

// Wine console stdout is unreliable under `proton run` — log to a file next
// to the exe (cwd of the run).
static FILE *g_out;
#define printf(...) fprintf(g_out, __VA_ARGS__)

static void flush_log()
{
    if (g_out != nullptr) {
        fflush(g_out);
    }
}

typedef bool(__cdecl *VR_IsHmdPresent_t)();
typedef bool(__cdecl *VR_IsRuntimeInstalled_t)();
typedef bool(__cdecl *VR_IsInterfaceVersionValid_t)(const char *);
typedef uint32_t(__cdecl *VR_InitInternal2_t)(int32_t *, int32_t, const char *);
typedef int32_t(__cdecl *VR_InitInternal_t)(int32_t *, int32_t);
typedef void *(__cdecl *VR_GetGenericInterface_t)(const char *, int32_t *);
typedef const char *(__cdecl *VR_GetVRInitErrorAsEnglishDescription_t)(int32_t);

static void mod_path(HMODULE m, const char *tag)
{
    wchar_t w[MAX_PATH];
    char a[MAX_PATH * 2];
    if (m == nullptr || GetModuleFileNameW(m, w, MAX_PATH) == 0) {
        printf("%s: <not loaded>\n", tag);
        return;
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, a, sizeof(a), nullptr, nullptr);
    printf("%s: %s\n", tag, n > 0 ? a : "?");
}

static void dump_loaded(const char *needle)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return;
    }
    MODULEENTRY32W me;
    me.dwSize = sizeof(me);
    char a[MAX_PATH * 2];
    if (Module32FirstW(snap, &me)) {
        do {
            WideCharToMultiByte(CP_UTF8, 0, me.szExePath, -1, a, sizeof(a),
                                nullptr, nullptr);
            if (strstr(a, needle) != nullptr) {
                printf("  loaded module: %s\n", a);
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
}

int main()
{
    g_out = fopen("openvr_probe_out.txt", "w");
    if (g_out != nullptr) {
        setvbuf(g_out, nullptr, _IONBF, 0);
    } else {
        g_out = stderr;
    }
    printf("=== mc2vr openvr probe (pid=%lu) ===\n", GetCurrentProcessId());
    const char *app_id = getenv("SteamAppId");
    const char *steam_path = getenv("SteamPath");
    printf("env: SteamAppId=%s SteamPath=%s\n", app_id ? app_id : "<unset>",
           steam_path ? steam_path : "<unset>");

    HMODULE api = LoadLibraryW(L"openvr_api_dxvk.dll");
    if (api == nullptr) {
        printf("FATAL: openvr_api_dxvk.dll load failed (%lu)\n", GetLastError());
        return 1;
    }
    mod_path(api, "openvr_api_dxvk");

    auto p_hmd = (VR_IsHmdPresent_t)GetProcAddress(api, "VR_IsHmdPresent");
    auto p_rt = (VR_IsRuntimeInstalled_t)GetProcAddress(
        api, "VR_IsRuntimeInstalled");
    auto p_valid = (VR_IsInterfaceVersionValid_t)GetProcAddress(
        api, "VR_IsInterfaceVersionValid");
    auto p_init2 = (VR_InitInternal2_t)GetProcAddress(api, "VR_InitInternal2");
    auto p_init1 = (VR_InitInternal_t)GetProcAddress(api, "VR_InitInternal");
    auto p_iface = (VR_GetGenericInterface_t)GetProcAddress(
        api, "VR_GetGenericInterface");
    auto p_desc = (VR_GetVRInitErrorAsEnglishDescription_t)GetProcAddress(
        api, "VR_GetVRInitErrorAsEnglishDescription");
    printf("exports: hmd=%p rt=%p valid=%p init2=%p init1=%p iface=%p\n",
           (void *)p_hmd, (void *)p_rt, (void *)p_valid, (void *)p_init2,
           (void *)p_init1, (void *)p_iface);

    printf("pre-init: runtime installed=%d hmd present=%d\n", p_rt(),
           p_hmd());

    // Attempt 0: call vrclient_init_registry (the intended prefix-VR setup
    // writer — loads Vulkan, connects to the compositor, and writes the
    // Software\Wine\VR values that vrclient/DXVK hard-require). On normal
    // Steam launches something invokes this during VR prefix preparation;
    // direct `proton run` never does.
    {
        HMODULE vrc = LoadLibraryW(L"vrclient.dll");
        if (vrc != nullptr) {
            typedef int(__cdecl *init_registry_t)(void *);
            auto p_initreg = (init_registry_t)GetProcAddress(
                vrc, "vrclient_init_registry");
            printf("attempt 0: vrclient_init_registry = %p\n", (void *)p_initreg);
            if (p_initreg != nullptr) {
                // Params struct layout unknown — pass a generously zeroed
                // buffer and hope the function only fills result fields.
                static unsigned char params[256];
                int rc = p_initreg(params);
                printf("attempt 0: vrclient_init_registry -> %d\n", rc);
            }
        } else {
            printf("attempt 0: vrclient.dll load failed (%lu)\n", GetLastError());
        }
        // Read back what (if anything) it wrote — try both hives/views.
        char val[1024] = {0};
        DWORD size = sizeof(val);
        if (RegGetValueA(HKEY_LOCAL_MACHINE, "Software\\Wine\\VR",
                        "openvr_vulkan_instance_extensions", RRF_RT_REG_SZ,
                        nullptr, val, &size) == ERROR_SUCCESS) {
            printf("HKLM Software\\Wine\\VR = \"%s\"\n", val);
        } else {
            printf("HKLM Software\\Wine\\VR value missing\n");
        }
    }

    int32_t err = 0;
    uint32_t token = 0;
    int stage = 0;

    // 1: VR_InitInternal2, Scene
    token = p_init2(&err, 1, "mc2vr-probe");
    printf("attempt 1 (InitInternal2, Scene): err=%d (%s) token=%u\n", err,
           p_desc(err), token);
    if (err == 0) {
        stage = 1;
    } else {
        dump_loaded("vrclient");
        // 2: legacy VR_InitInternal, Scene
        err = 0;
        token = (uint32_t)p_init1(&err, 1);
        printf("attempt 2 (InitInternal, Scene): err=%d (%s) token=%u\n", err,
               p_desc(err), token);
        if (err == 0) {
            stage = 2;
        }
    }

    if (err != 0) {
        dump_loaded("vrclient");
        // 3: Background — no compositor/HMD requirement
        err = 0;
        token = p_init2(&err, 3, "mc2vr-probe");
        printf("attempt 3 (InitInternal2, Background): err=%d (%s) token=%u\n",
               err, p_desc(err), token);
        if (err == 0) {
            stage = 3;
        }
    }

    if (err != 0) {
        // 4: preload the runtime's own vrclient explicitly, retry Scene
        HMODULE vrc = LoadLibraryW(L"C:\\vrclient\\bin\\vrclient.dll");
        mod_path(vrc, "preload C:\\vrclient\\bin\\vrclient.dll");
        if (vrc != nullptr) {
            err = 0;
            token = p_init2(&err, 1, "mc2vr-probe");
            printf("attempt 4 (InitInternal2, Scene, after preload): err=%d "
                   "(%s) token=%u\n",
                   err, p_desc(err), token);
            if (err == 0) {
                stage = 4;
            }
        }
    }

    dump_loaded("vrclient");
    dump_loaded("openvr");

    if (err != 0) {
        printf("=== probe result: ALL INIT ATTEMPTS FAILED ===\n");
        return 2;
    }

    printf("=== probe result: CONNECTED (stage %d) ===\n", stage);
    const char *probes[] = {"IVRSystem_026", "IVRSystem_022",
                            "IVRCompositor_029", "IVRCompositor_022"};
    for (const char *v : probes) {
        printf("version %s valid=%d\n", v, p_valid(v));
    }

    int32_t ie = 0;
    void *sys = p_iface("FnTable:IVRSystem_022", &ie);
    printf("FnTable:IVRSystem_022 -> %p (err=%d)\n", sys, ie);
    if (sys != nullptr && ie == 0) {
        // 1.16.8 IVRSystem_022 layout (pinned header — do NOT use master):
        // slot 0 GetRecommendedRenderTargetSize(self,w,h), slot 2
        // GetProjectionRaw, slot 6 GetD3D9AdapterIndex(self), slot 11
        // GetDeviceToAbsoluteTrackingPose(self,origin,pred,poses,count),
        // slot 27 GetStringTrackedDeviceProperty(self,idx,prop,buf,size,err)
        struct Fn {
            void *slot[28];
        } *fn = (Fn *)sys;
        uint32_t w = 0, h = 0;
        ((void(__cdecl *)(void *, uint32_t *, uint32_t *))fn->slot[0])(
            fn, &w, &h);
        printf("recommended target: %ux%u\n", w, h);
        printf("D3D9 adapter: %d\n",
               ((int(__cdecl *)(void *))fn->slot[6])(fn));
        float pl, pr, pt, pb;
        ((void(__cdecl *)(void *, int, float *, float *, float *, float *))
             fn->slot[2])(fn, 0, &pl, &pr, &pt, &pb);
        printf("proj raw L: [%.4f %.4f %.4f %.4f]\n", pl, pr, pt, pb);
        char buf[256] = {0};
        int32_t pe = 0;
        ((uint32_t(__cdecl *)(void *, uint32_t, int32_t, char *, uint32_t,
                              int32_t *))fn->slot[27])(
            fn, 0, 1005 /*Prop_ManufacturerName_String*/, buf, sizeof(buf),
            &pe);
        printf("HMD manufacturer: \"%s\" (err=%d)\n", buf, pe);
        struct Pose {
            float m[3][4];
            float vel[3];
            float avel[3];
            int32_t result;
            uint8_t valid;
            uint8_t connected;
        } pose;
        memset(&pose, 0, sizeof(pose));
        ((void(__cdecl *)(void *, int, float, void *, uint32_t))fn->slot[11])(
            fn, 1 /*Standing*/, 0.0f, &pose, 1);
        printf("pose: valid=%d connected=%d result=%d\n", pose.valid,
               pose.connected, pose.result);
        printf("pose m = [%.3f %.3f %.3f %.3f; %.3f %.3f %.3f %.3f; "
               "%.3f %.3f %.3f %.3f]\n",
               pose.m[0][0], pose.m[0][1], pose.m[0][2], pose.m[0][3],
               pose.m[1][0], pose.m[1][1], pose.m[1][2], pose.m[1][3],
               pose.m[2][0], pose.m[2][1], pose.m[2][2], pose.m[2][3]);
    }

    // Fetch the VR Vulkan instance extensions the compositor requires —
    // this is the content the HKLM\Software\Wine\VR "openvr_vulkan_instance_"
    //extensions" registry value must carry for the Proton 20261001 vrclient
    // (and DXVK) to initialize. IVRCompositor_022 FnTable slot 40 =
    // GetVulkanInstanceExtensionsRequired(self, char*, uint32).
    void *comp = p_iface("FnTable:IVRCompositor_022", &ie);
    printf("FnTable:IVRCompositor_022 -> %p (err=%d)\n", comp, ie);
    if (comp != nullptr && ie == 0) {
        struct CompFn {
            void *slot[41];
        } *cf = (CompFn *)comp;
        // 1.16.8 IVRCompositor layout (no SubmitWithArrayIndex in this
        // version!): GetVulkanInstanceExtensionsRequired = slot 39,
        // GetVulkanDeviceExtensionsRequired = slot 40.
        printf("comp slot 39 (instance ext) = %p\n", cf->slot[39]);
        char ext[1024] = {0};
        if (cf->slot[39] != nullptr) {
            ((uint32_t(__cdecl *)(void *, char *, uint32_t))cf->slot[39])(
                cf, ext, sizeof(ext));
            printf("instance extensions: \"%s\"\n", ext);
        }
    }
    return 0;
}
