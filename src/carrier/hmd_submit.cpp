// S4-2: per-eye LDR capture + SteamVR compositor submit. See hmd_submit.hpp.

#include "hmd_submit.hpp"

#include <windows.h>
#include <d3d9.h> // vtable type/layout constants only

#include "device.hpp"
#include "hooks.hpp"
#include "log.hpp"
#include "openvr_bridge.hpp"

#include <cstring>

namespace mc2vr::submit {

namespace {

// Device/texture/surface vtable slots (i386 d3d9.h order; indices pinned by
// M2/M4 runtime evidence — eye_replay.cpp uses the same anchors:
// GetBackBuffer=18, CreateRenderTarget=28, GetRenderTargetData=32,
// CreateOffscreenPlainSurface=36, surface GetDesc=12).
constexpr size_t DSLOT_CreateTexture = 23;   // IDirect3DDevice9::CreateTexture
constexpr size_t TSLOT_GetSurfaceLevel = 18; // IDirect3DTexture9::GetSurfaceLevel
constexpr size_t SLOT_Release = 2;           // IUnknown, both objects

using SurfaceDesc_t = HRESULT(__stdcall *)(void *, D3DSURFACE_DESC *);
using Release_t = HRESULT(__stdcall *)(void *);

bool g_enabled = false;

// Capture state (render thread only; textures are DEFAULT pool — lost on
// Reset, dropped in on_reset and re-created lazily).
void *g_device = nullptr;
void *g_backbuffer = nullptr;
void *g_tex[2] = {nullptr, nullptr};   // index = vr::EVREye (0=L, 1=R)
void *g_surf[2] = {nullptr, nullptr};  // tex level-0 surfaces (AddRef'd)
bool g_tex_failed = false;             // stop retrying after a failed create
bool g_left_fresh = false;             // LEFT captured this frame, awaiting R

// ---- window counters ----------------------------------------------------------

uint64_t g_captures[2] = {0, 0};
uint64_t g_capture_fails = 0;
uint64_t g_submits = 0;      // Submit calls attempted (both eyes)
uint64_t g_submit_ok = 0;   // Submit calls that returned None
uint64_t g_skip_no_comp = 0; // boundaries skipped: compositor not ready
uint64_t g_skip_no_left = 0; // 2->0 boundaries skipped: no fresh LEFT
uint64_t g_skip_throttled = 0; // boundaries skipped: failure backoff window
int32_t g_last_err[2] = {1, 1}; // last Submit result per eye (1 = never ok)
bool g_first_logged = false;

// Failure backoff (run 7 lesson): an unfocused submit pair costs ~14 ms of
// the 16 ms frame budget in IPC (Submit throttles to 10 Hz DoNotHaveFocus
// — observed 6-8 ms per call). After a FAILED pair, retry at most every
// 500 ms; a successful pair clears the backoff and resumes full rate.
// Pure carrier-side timing, no new OpenVR calls.
LARGE_INTEGER g_backoff_until = {}; // 0 = no backoff

bool submit_due()
{
    if (g_backoff_until.QuadPart == 0) {
        return true;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart >= g_backoff_until.QuadPart;
}

void arm_backoff()
{
    static LARGE_INTEGER freq = {};
    LARGE_INTEGER now;
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    QueryPerformanceCounter(&now);
    g_backoff_until.QuadPart = now.QuadPart + freq.QuadPart / 2; // 0.5 s
}

const char *comp_err_name(int32_t e)
{
    switch (e) {
    case vr::VRCompositorError_None: return "None";
    case vr::VRCompositorError_RequestFailed: return "RequestFailed";
    case vr::VRCompositorError_IncompatibleVersion: return "IncompatibleVersion";
    case vr::VRCompositorError_DoNotHaveFocus: return "DoNotHaveFocus";
    case vr::VRCompositorError_InvalidTexture: return "InvalidTexture";
    case vr::VRCompositorError_IsNotSceneApplication: return "IsNotSceneApplication";
    case vr::VRCompositorError_TextureIsOnWrongDevice: return "TextureIsOnWrongDevice";
    case vr::VRCompositorError_TextureUsesUnsupportedFormat:
        return "TextureUsesUnsupportedFormat";
    case vr::VRCompositorError_SharedTexturesNotSupported:
        return "SharedTexturesNotSupported";
    case vr::VRCompositorError_IndexOutOfRange: return "IndexOutOfRange";
    case vr::VRCompositorError_AlreadySubmitted: return "AlreadySubmitted";
    case vr::VRCompositorError_InvalidBounds: return "InvalidBounds";
    default: return "unknown-code";
    }
}

// Create the two capture textures from the backbuffer's own desc (size+format
// match the swapchain: X8R8G8B8-class LDR). DEFAULT pool + RENDERTARGET usage:
// the compositor consumes a device texture (the Proton DXVK interop path), and
// StretchRect requires the destination to be a render target.
bool ensure_textures()
{
    if (g_tex[0] && g_tex[1]) {
        return true;
    }
    if (g_tex_failed || !g_device || !g_backbuffer) {
        return false;
    }

    D3DSURFACE_DESC desc = {};
    if (FAILED(((SurfaceDesc_t)(*(void ***)g_backbuffer)[12])(g_backbuffer,
                                                              &desc))) {
        g_tex_failed = true;
        MC2VR_LOG("ovr submit: backbuffer GetDesc FAILED — capture inactive "
                  "this run");
        return false;
    }

    void **vt = *(void ***)g_device;
    auto create = (HRESULT(__stdcall *)(void *, UINT, UINT, UINT, DWORD,
                                        D3DFORMAT, D3DPOOL, void **,
                                        void **))vt[DSLOT_CreateTexture];
    for (int eye = 0; eye < 2; eye++) {
        void *tex = nullptr;
        HRESULT hr = create(g_device, desc.Width, desc.Height, 1,
                            D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT,
                            &tex, nullptr);
        if (FAILED(hr) || !tex) {
            MC2VR_LOG("ovr submit: CreateTexture(%ux%u fmt=%u) FAILED "
                      "hr=%08lx — capture inactive this run",
                      desc.Width, desc.Height, (unsigned)desc.Format,
                      (unsigned long)hr);
            g_tex_failed = true;
            return false;
        }
        void *surf = nullptr;
        hr = ((HRESULT(__stdcall *)(void *, UINT, void **))
                  (*(void ***)tex)[TSLOT_GetSurfaceLevel])(tex, 0, &surf);
        if (FAILED(hr) || !surf) {
            MC2VR_LOG("ovr submit: GetSurfaceLevel FAILED hr=%08lx",
                      (unsigned long)hr);
            ((Release_t)(*(void ***)tex)[SLOT_Release])(tex);
            g_tex_failed = true;
            return false;
        }
        g_tex[eye] = tex;
        g_surf[eye] = surf;
    }
    MC2VR_LOG("ovr submit: capture textures created %ux%u fmt=%u "
              "(DEFAULT pool, RT usage)",
              desc.Width, desc.Height, (unsigned)desc.Format);
    return true;
}

void release_textures()
{
    for (int eye = 0; eye < 2; eye++) {
        if (g_surf[eye]) {
            ((Release_t)(*(void ***)g_surf[eye])[SLOT_Release])(g_surf[eye]);
            g_surf[eye] = nullptr;
        }
        if (g_tex[eye]) {
            ((Release_t)(*(void ***)g_tex[eye])[SLOT_Release])(g_tex[eye]);
            g_tex[eye] = nullptr;
        }
    }
    g_tex_failed = false;
    g_left_fresh = false;
}

// Backbuffer -> capture texture surface, through the original StretchRect
// trampoline (same proven path as the monitor pin; bypasses the hook chain).
bool capture(int eye)
{
    if (!device::blit_surfaces(g_backbuffer, g_surf[eye])) {
        g_capture_fails++;
        return false;
    }
    g_captures[eye]++;
    return true;
}

} // namespace

bool set_enabled(const char *value)
{
    if (strcmp(value, "on") == 0) {
        g_enabled = true;
    } else if (strcmp(value, "off") == 0) {
        g_enabled = false;
    } else {
        return false;
    }
    MC2VR_LOG("ovr submit: openvr_submit=%s (per-eye capture at pass "
              "boundaries + IVRCompositor::Submit)",
              g_enabled ? "on" : "off");
    return true;
}

void on_boundary(void *device, uint32_t prev_pass, uint32_t next_pass,
                 void *backbuffer)
{
    if (!g_enabled) {
        return;
    }

    // The compositor pointer is published by the bootstrap thread; a torn
    // read would be a pointer-sized aligned store — benign (we re-read next
    // boundary), and in practice bootstrap finishes long before gameplay.
    auto *comp = ovr::compositor();
    if (comp == nullptr) {
        g_skip_no_comp++;
        return;
    }

    g_device = device;
    g_backbuffer = backbuffer;

    if (prev_pass == 1 && next_pass == 2) {
        // Pass 1 just finished: the backbuffer holds its final LEFT
        // composite (the per-frame Present that would consume it runs at
        // the START of pass 2's submit).
        if (ensure_textures() && capture(0)) {
            g_left_fresh = true;
        }
        return;
    }

    if (prev_pass == 2 && next_pass == 0) {
        // Pass 2 just finished: the backbuffer holds its final RIGHT
        // composite — capture it BEFORE the caller's monitor-pin restore
        // overwrites the backbuffer.
        if (!ensure_textures() || !g_left_fresh) {
            if (!g_left_fresh) {
                g_skip_no_left++;
            }
            return;
        }
        if (!capture(1)) {
            g_left_fresh = false;
            return;
        }
        g_left_fresh = false;

        // Failure backoff: skip the (expensive, throttled) submit pair while
        // the previous pair failed — the captures above still run, so the
        // textures stay fresh for the moment focus arrives.
        if (!submit_due()) {
            g_skip_throttled++;
            return;
        }

        // Submit the pair. TextureType_DirectX with an IDirect3DTexture9*
        // is openvr_api_dxvk.dll's intended 32-bit D3D9 path; the backbuffer
        // is gamma-space LDR (the game's own tonemapped composite), so
        // ColorSpace_Gamma. Enter-log the FIRST pair so any hang pinpoints
        // the call (run-5 discipline).
        vr::Texture_t tl = {};
        tl.handle = g_tex[0];
        tl.eType = vr::TextureType_DirectX;
        tl.eColorSpace = vr::ColorSpace_Gamma;
        vr::Texture_t tr = {};
        tr.handle = g_tex[1];
        tr.eType = vr::TextureType_DirectX;
        tr.eColorSpace = vr::ColorSpace_Gamma;

        if (!g_first_logged) {
            g_first_logged = true;
            MC2VR_LOG("ovr submit: first Submit pair (frame=%llu, "
                      "L=%p R=%p)",
                      (unsigned long long)hooks::frame_count(), g_tex[0],
                      g_tex[1]);
        }
        const int32_t el = (int32_t)comp->Submit(vr::Eye_Left, &tl, nullptr,
                                                 vr::Submit_Default);
        const int32_t er = (int32_t)comp->Submit(vr::Eye_Right, &tr, nullptr,
                                                 vr::Submit_Default);
        g_submits += 2;
        if (el == vr::VRCompositorError_None) {
            g_submit_ok++;
        }
        if (er == vr::VRCompositorError_None) {
            g_submit_ok++;
        }
        if (el == vr::VRCompositorError_None &&
            er == vr::VRCompositorError_None) {
            g_backoff_until.QuadPart = 0; // full rate while submitting ok
        } else {
            arm_backoff();
        }
        // Log every DISTINCT result per eye (once per state change — no
        // per-frame spam at 30 fps).
        if (el != g_last_err[0]) {
            MC2VR_LOG("ovr submit: LEFT Submit -> %d (%s)", el,
                      comp_err_name(el));
            g_last_err[0] = el;
        }
        if (er != g_last_err[1]) {
            MC2VR_LOG("ovr submit: RIGHT Submit -> %d (%s)", er,
                      comp_err_name(er));
            g_last_err[1] = er;
        }

        // No WaitGetPoses in this first cut: at 30 fps game vs 120 Hz HMD the
        // blocking running-start wait would re-pace the game loop. If the
        // runtime demands it the submits will come back AlreadySubmitted and
        // the run tells us to add it (decision table, docs/s4_handover.md).
        //
        // NO PostPresentHandoff — RUN 7 (2026-10-04) LIVE-PROVEN CRASH
        // SUSPECT: the game died ~20 ms after the first unfocused submit
        // pair (both Submit -> 101 DoNotHaveFocus, clean IPC round-trips),
        // and PostPresentHandoff was the very next call — never logged, never
        // previously exercised, and documented to "access the Vulkan queue",
        // invoked mid-frame (inside the game's SubmitToGPU) with DXVK's
        // boot-time interop deliberately disarmed. It is OPTIONAL per
        // openvr.h (only needed when the app can't call WaitGetPoses right
        // after Present, which we don't call yet); revisit only together
        // with the WaitGetPoses/pacing design.
    }
}

void on_reset()
{
    if (!g_enabled) {
        return;
    }
    release_textures();
    g_backbuffer = nullptr;
    MC2VR_LOG("ovr submit: Reset — capture textures dropped, re-created lazily");
}

void report_window()
{
    if (!g_enabled) {
        return;
    }
    MC2VR_LOG("ovr submit window: captures L=%llu R=%llu capFails=%llu "
              "submits=%llu ok=%llu lastErr L=%d(%s) R=%d(%s) "
              "skipNoComp=%llu skipNoLeft=%llu skipThrottled=%llu",
              (unsigned long long)g_captures[0],
              (unsigned long long)g_captures[1],
              (unsigned long long)g_capture_fails,
              (unsigned long long)g_submits, (unsigned long long)g_submit_ok,
              g_last_err[0], comp_err_name(g_last_err[0]), g_last_err[1],
              comp_err_name(g_last_err[1]),
              (unsigned long long)g_skip_no_comp,
              (unsigned long long)g_skip_no_left,
              (unsigned long long)g_skip_throttled);
    g_captures[0] = g_captures[1] = 0;
    g_capture_fails = 0;
    g_submits = 0;
    g_submit_ok = 0;
    g_skip_no_comp = 0;
    g_skip_no_left = 0;
    g_skip_throttled = 0;
}

} // namespace mc2vr::submit
