// S4-2c probe B: the helper core — a first-class 64-bit D3D11 VR app.
//
// Proves the helper half of the relay design (docs/s4_handover.md §S4 option
// C) end-to-end WITHOUT the game: 64-bit wine process, DXVK D3D11 device,
// 64-bit openvr_api_dxvk.dll, textbook loop
//   WaitGetPoses -> Submit(L) -> Submit(R) -> PostPresentHandoff
// putting a test pattern in the HMD for ~15 s. LEFT is red-tinted, RIGHT
// cyan-tinted — close one eye at a time to verify stereo separation.
//
// This is the SteamVR/Proton-supported app class (what every 64-bit VR game
// under Proton is): the boot-time DXVK interop is INTENTIONALLY armed here
// (registry values written before device creation; SteamVR must be up —
// this is the helper, not the game, so the run-6a landmine does not apply:
// nothing of the game is at stake if SteamVR is mid-transition).
//
// Build: x86_64-w64-mingw32-g++ -static -mconsole -O2 -o helper64.exe helper64.cpp -ld3d11 -ladvapi32
// Run:   <proton>/proton run helper64.exe   (SteamVR running, HMD awake)
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

static FILE *g_out;
#define printf(...) fprintf(g_out, __VA_ARGS__)
static void flush_log() { fflush(g_out); }

// ---- OpenVR flat API (openvr_api_dxvk.dll, extern "C") ------------------------

typedef bool(__cdecl *VR_IsInterfaceVersionValid_t)(const char *);
typedef uint32_t(__cdecl *VR_InitInternal2_t)(int32_t *, int32_t, const char *);
typedef void(__cdecl *VR_ShutdownInternal_t)();
typedef void *(__cdecl *VR_GetGenericInterface_t)(const char *, int32_t *);
typedef const char *(__cdecl *VR_GetVRInitErrorAsEnglishDescription_t)(int32_t);

// FnTable slots (IVRSystem_022 / IVRCompositor_022 order — same pinned
// 1.16.8 layout the 32-bit carrier verified against the builtin builder;
// on x64 the entries are plain function pointers).
struct SystemFn {
    void *slot[28];
};
struct CompositorFn {
    void *slot[46];
};
constexpr size_t SYS_GetRecommendedRenderTargetSize = 0;
constexpr size_t SYS_GetDeviceToAbsoluteTrackingPose = 11;
constexpr size_t SYS_GetTrackedDeviceClass = 19;
constexpr size_t SYS_GetFloatTrackedDeviceProperty = 22;
constexpr size_t COMP_WaitGetPoses = 2;
constexpr size_t COMP_Submit = 5;
constexpr size_t COMP_PostPresentHandoff = 7;
constexpr size_t COMP_GetVulkanInstanceExtensionsRequired = 39;

enum { Eye_Left = 0, Eye_Right = 1 };
enum { VRApplication_Scene = 1, VRInitError_None = 0 };
enum { TextureType_DirectX = 0, ColorSpace_Gamma = 1, Submit_Default = 0 };

struct PoseT {
    float m[3][4];
    float vVelocity[3];
    float vAngularVelocity[3];
    int32_t eTrackingResult;
    uint8_t bPoseIsValid;
    uint8_t bDeviceIsConnected;
}; // 80 bytes with padding — matches TrackedDevicePose_t
constexpr uint32_t MAX_DEVICES = 64;

struct TextureT { // vr::Texture_t, pack(8) x64 layout (24 bytes)
    int32_t eType;
    void *handle;
    int32_t eColorSpace;
};

// ---- err-105 registry fix (same three values the carrier writes; the
// helper arms its OWN interop at device creation — intended for VR apps) --

static bool ensure_vr_registry()
{
    HKEY key = nullptr;
    DWORD disp = 0;
    LSTATUS rc = RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\Wine\\VR", 0,
                                 nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                                 &disp);
    if (rc != ERROR_SUCCESS) {
        printf("RegCreateKeyExA(Software\\Wine\\VR) failed %lu\n", rc);
        return false;
    }
    char rt[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("PROTON_VR_RUNTIME", rt, sizeof(rt));
    if (n == 0 || n >= sizeof(rt)) {
        RegCloseKey(key);
        printf("PROTON_VR_RUNTIME missing\n");
        return false;
    }
    bool ok = true;
    ok &= RegSetValueExA(key, "PROTON_VR_RUNTIME", 0, REG_SZ,
                         (const BYTE *)rt, n) == ERROR_SUCCESS;
    const DWORD state = 1;
    ok &= RegSetValueExA(key, "state", 0, REG_DWORD, (const BYTE *)&state,
                         sizeof(state)) == ERROR_SUCCESS;
    ok &= RegSetValueExA(key, "openvr_vulkan_instance_extensions", 0, REG_SZ,
                         (const BYTE *)"", 1) == ERROR_SUCCESS;
    RegCloseKey(key);
    printf("VR registry values written (disp=%lu, %s) — DXVK d3d11 interop "
           "will arm at device creation\n", disp, ok ? "ok" : "PARTIAL");
    return ok;
}

static ID3D11Device *g_dev;
static ID3D11DeviceContext *g_ctx;
static ID3D11Texture2D *g_tex[2];

static bool make_eye_texture(int eye, uint8_t tint_r, uint8_t tint_g,
                             uint8_t tint_b)
{
    const UINT W = 1024, H = 1024;
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = W;
    desc.Height = H;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;

    D3D11_TEXTURE2D_DESC st_desc = desc;
    st_desc.Usage = D3D11_USAGE_STAGING;
    st_desc.BindFlags = 0;
    st_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    ID3D11Texture2D *staging = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&st_desc, nullptr, &staging))) {
        printf("FATAL: staging create failed (eye %d)\n", eye);
        return false;
    }
    D3D11_MAPPED_SUBRESOURCE map = {};
    if (FAILED(g_ctx->Map(staging, 0, D3D11_MAP_WRITE, 0, &map))) {
        printf("FATAL: staging map failed (eye %d)\n", eye);
        return false;
    }
    for (UINT y = 0; y < H; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)map.pData + y * map.RowPitch);
        for (UINT x = 0; x < W; x++) {
            // Checkerboard + gradient, tinted per eye so L/R are visibly
            // distinct through the lenses.
            const uint8_t check = ((x / 64) + (y / 64)) % 2 ? 255 : 96;
            const uint8_t r = (uint8_t)((tint_r * check) / 255);
            const uint8_t g = (uint8_t)((tint_g * check) / 255);
            const uint8_t b = (uint8_t)((tint_b * check) / 255);
            row[x] = ((uint32_t)255 << 24) | ((uint32_t)r << 16) |
                     ((uint32_t)g << 8) | b;
        }
    }
    g_ctx->Unmap(staging, 0);

    bool ok = SUCCEEDED(g_dev->CreateTexture2D(&desc, nullptr, &g_tex[eye]));
    if (ok) {
        g_ctx->CopyResource(g_tex[eye], staging);
    }
    staging->Release();
    printf("eye %d texture created%s\n", eye, ok ? "" : " FAILED");
    return ok;
}

int main()
{
    g_out = fopen("helper64.log", "w");
    printf("=== helper64 probe starting (64-bit wine VR app) ===\n");

    ensure_vr_registry(); // BEFORE device creation — arms DXVK's interop

    // ---- D3D11 device -----------------------------------------------------
    HMODULE d3d11 = LoadLibraryA("d3d11.dll");
    if (!d3d11) {
        printf("FATAL: d3d11.dll not found\n");
        return 1;
    }
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                    0, nullptr, 0, D3D11_SDK_VERSION, &g_dev,
                                    nullptr, &g_ctx);
    if (FAILED(hr) || !g_dev) {
        printf("FATAL: D3D11CreateDevice hr=%08lx — is SteamVR up? (armed "
               "interop connects at device creation)\n", (unsigned long)hr);
        return 1;
    }
    printf("D3D11 device created (armed VR interop path)\n");

    if (!make_eye_texture(Eye_Left, 255, 60, 60) ||
        !make_eye_texture(Eye_Right, 60, 255, 255)) {
        return 1;
    }

    // ---- OpenVR connect -----------------------------------------------------
    HMODULE vr = LoadLibraryA("openvr_api_dxvk.dll");
    if (!vr) {
        printf("FATAL: openvr_api_dxvk.dll not found (64-bit system32)\n");
        return 1;
    }
    auto VR_InitInternal2 = (VR_InitInternal2_t)GetProcAddress(vr, "VR_InitInternal2");
    auto VR_ShutdownInternal = (VR_ShutdownInternal_t)GetProcAddress(vr, "VR_ShutdownInternal");
    auto VR_GetGenericInterface = (VR_GetGenericInterface_t)GetProcAddress(vr, "VR_GetGenericInterface");
    auto VR_IsInterfaceVersionValid = (VR_IsInterfaceVersionValid_t)GetProcAddress(vr, "VR_IsInterfaceVersionValid");
    auto VR_GetVRInitErrorAsEnglishDescription =
        (VR_GetVRInitErrorAsEnglishDescription_t)GetProcAddress(vr, "VR_GetVRInitErrorAsEnglishDescription");
    if (!VR_InitInternal2 || !VR_ShutdownInternal || !VR_GetGenericInterface ||
        !VR_IsInterfaceVersionValid || !VR_GetVRInitErrorAsEnglishDescription) {
        printf("FATAL: openvr_api_dxvk exports missing\n");
        return 1;
    }
    printf("openvr_api_dxvk.dll (x64) loaded\n");

    int32_t err = VRInitError_None;
    uint32_t token = 0;
    for (int attempt = 1;; attempt++) {
        err = VRInitError_None;
        token = VR_InitInternal2(&err, VRApplication_Scene, "mc2vr-helper");
        if (err == VRInitError_None) {
            break;
        }
        printf("VR_InitInternal2 attempt %d failed err=%d (%s)\n", attempt, err,
               VR_GetVRInitErrorAsEnglishDescription(err));
        if (attempt >= 5) {
            printf("FATAL: giving up on SteamVR\n");
            return 1;
        }
        Sleep(4000);
    }
    printf("VR_InitInternal2 ok (token=%u) — SteamVR connected\n", token);
    Sleep(2000); // settle: a Scene connect wakes a standby HMD (probe-proven)

    if (!VR_IsInterfaceVersionValid("IVRSystem_022") ||
        !VR_IsInterfaceVersionValid("IVRCompositor_022")) {
        printf("FATAL: 022 interfaces not served to the 64-bit client\n");
        return 1;
    }
    int32_t iface_err = 0;
    SystemFn *sys = (SystemFn *)VR_GetGenericInterface("FnTable:IVRSystem_022",
                                                       &iface_err);
    CompositorFn *comp =
        (CompositorFn *)VR_GetGenericInterface("FnTable:IVRCompositor_022",
                                                &iface_err);
    if (!sys || !comp) {
        printf("FATAL: FnTables unavailable (err=%d)\n", iface_err);
        return 1;
    }
    printf("FnTable:IVRSystem_022 + FnTable:IVRCompositor_022 acquired\n");

    uint32_t w = 0, h = 0;
    ((void(__cdecl *)(SystemFn *, uint32_t *, uint32_t *))
         sys->slot[SYS_GetRecommendedRenderTargetSize])(sys, &w, &h);
    const int32_t dev_class =
        ((int32_t(__cdecl *)(SystemFn *, uint32_t))
             sys->slot[SYS_GetTrackedDeviceClass])(sys, 0);
    printf("recommended RT %ux%u, device 0 class=%d (want 1)\n", w, h, dev_class);
    if (dev_class != 1) {
        printf("FATAL: device 0 is not the HMD\n");
        return 1;
    }

    // ---- The textbook frame loop ------------------------------------------
    static PoseT render_poses[MAX_DEVICES];
    static PoseT game_poses[MAX_DEVICES];
    TextureT tl = {TextureType_DirectX, g_tex[Eye_Left], ColorSpace_Gamma};
    TextureT tr = {TextureType_DirectX, g_tex[Eye_Right], ColorSpace_Gamma};

    int32_t last_l = -99, last_r = -99;
    uint64_t frames = 0, ok_l = 0, ok_r = 0;
    printf("loop: WaitGetPoses -> Submit L/R -> PostPresentHandoff for 15 s "
           "(put the HMD on)\n");
    flush_log();

    const ULONGLONG t0 = GetTickCount64();
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        if (now - t0 > 15000) {
            break;
        }

        ((int32_t(__cdecl *)(CompositorFn *, PoseT *, uint32_t, PoseT *,
                             uint32_t))comp->slot[COMP_WaitGetPoses])(
            comp, render_poses, MAX_DEVICES, game_poses, MAX_DEVICES);

        const int32_t el =
            ((int32_t(__cdecl *)(CompositorFn *, int32_t, const TextureT *,
                                 const void *, int32_t))comp->slot[COMP_Submit])(
                comp, Eye_Left, &tl, nullptr, Submit_Default);
        const int32_t er =
            ((int32_t(__cdecl *)(CompositorFn *, int32_t, const TextureT *,
                                 const void *, int32_t))comp->slot[COMP_Submit])(
                comp, Eye_Right, &tr, nullptr, Submit_Default);
        frames++;
        if (el == 0) ok_l++;
        if (er == 0) ok_r++;
        if (el != last_l) {
            printf("Submit L -> %d\n", el);
            last_l = el;
        }
        if (er != last_r) {
            printf("Submit R -> %d\n", er);
            last_r = er;
        }

        ((void(__cdecl *)(CompositorFn *))comp->slot[COMP_PostPresentHandoff])(
            comp);

        if (frames == 1 || frames % 240 == 0) {
            const PoseT &hmd = render_poses[0];
            printf("frame %llu: hmd valid=%d pos=[%.2f %.2f %.2f]\n",
                   (unsigned long long)frames, hmd.bPoseIsValid,
                   hmd.m[0][3], hmd.m[1][3], hmd.m[2][3]);
            flush_log();
        }
    }

    printf("RESULT: %llu frames in 15 s (HMD-rate pacing works if > ~900); "
           "ok L=%llu R=%llu; last codes L=%d R=%d\n",
           (unsigned long long)frames, (unsigned long long)ok_l,
           (unsigned long long)ok_r, last_l, last_r);
    printf("RESULT: %s — look at the HMD: red-tinted LEFT / cyan-tinted RIGHT "
           "checkerboard = the helper path is proven end-to-end\n",
           (ok_l == frames && ok_r == frames) ? "PASS" : "PARTIAL");

    VR_ShutdownInternal();
    fclose(g_out);
    return 0;
}
