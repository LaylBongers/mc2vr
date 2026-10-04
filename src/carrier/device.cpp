#include "device.hpp"

#include <safetyhook.hpp>

#include <windows.h>
#include <d3d9.h> // types only; nothing here references Direct3DCreate9, so the carrier gains no d3d9.dll import

#include <cstdint>

#include "game_addresses.h"
#include "eye_replay.hpp"
#include "hooks.hpp"
#include "log.hpp"
#include "view_rewrite.hpp"

namespace mc2vr::device {

namespace {

// ---- Vtable slots ---------------------------------------------------------
// Header-derived (d3d9.h), VALIDATED against the game's observed anchor: the
// game applies render state through +0xe4 = slot 57, which is exactly
// SetRenderState's header slot (the SetDialogBoxMode-at-slot-20 quirk
// included). The Present-region slots below are then further confirmed at
// RUNTIME by call patterns — that confirmation is this milestone's
// deliverable: Present/EndScene/BeginScene fire exactly once per FrameTick,
// Reset stays silent unless device params change.
constexpr size_t SLOT_Release = 2;
constexpr size_t SLOT_GetSwapChain = 14;
constexpr size_t SLOT_Reset = 16;
constexpr size_t SLOT_Present = 17;
constexpr size_t SLOT_BeginScene = 41;
constexpr size_t SLOT_EndScene = 42;
// View rewrite channel: SetVertexShaderConstantF (slot 94 per the d3d9.h
// method order, which agrees with every runtime-pinned slot: Reset 16 /
// Present 17 / BeginScene 41 / EndScene 42 / SetRenderState 57). The draw
// camera reaches the GPU as VS constants (the engine is shader-driven;
// SetTransform is never called).
constexpr size_t SLOT_SetVertexShaderConstantF = 94;
// Observed (never altered) so the view rewrite knows which pass is drawing:
// shadow-map / reflection / other RT passes upload their own viewContextData
// and must not receive the eye shift.
constexpr size_t SLOT_SetRenderTarget = 37;
constexpr size_t SLOT_StretchRect = 34;
constexpr size_t SLOT_UpdateSurface = 30;
constexpr size_t SLOT_UpdateTexture = 31;
constexpr size_t SLOT_Surface_GetDesc = 12;
// IDirect3DSwapChain9::GetPresentParameters
constexpr size_t SLOT_SC_GetPresentParameters = 9;

using GetD3DDevice_t = void *(*)();

// COM x86 convention: __stdcall, this as first stack argument.
using Present_t = HRESULT(__stdcall *)(void *, const RECT *, const RECT *, HWND, const RGNDATA *);
using EndScene_t = HRESULT(__stdcall *)(void *);
using BeginScene_t = HRESULT(__stdcall *)(void *);
using Reset_t = HRESULT(__stdcall *)(void *, D3DPRESENT_PARAMETERS *);
using GetSwapChain_t = HRESULT(__stdcall *)(void *, UINT, void **);
using GetPresentParams_t = HRESULT(__stdcall *)(void *, D3DPRESENT_PARAMETERS *);
using SetRenderTarget_t = HRESULT(__stdcall *)(void *, DWORD, void *);
using StretchRect_t = HRESULT(__stdcall *)(void *, void *, const RECT *,
                                           void *, const RECT *, DWORD);
using UpdateSurface_t = HRESULT(__stdcall *)(void *, void *, void *, const RECT *,
                                             const POINT *);
using UpdateTexture_t = HRESULT(__stdcall *)(void *, void *, void *);
using SetVertexShaderConstantF_t = HRESULT(__stdcall *)(void *, UINT, const float *, UINT);

// Leaked by design (see device.hpp).
safetyhook::VmtHook *g_vmt_hook = nullptr;
safetyhook::VmHook *g_present_hook = nullptr;
safetyhook::VmHook *g_beginscene_hook = nullptr;
safetyhook::VmHook *g_endscene_hook = nullptr;
safetyhook::VmHook *g_reset_hook = nullptr;
safetyhook::VmHook *g_setvsconstf_hook = nullptr;
safetyhook::VmHook *g_setrt_hook = nullptr;
safetyhook::VmHook *g_stretchrect_hook = nullptr;
safetyhook::VmHook *g_updatesurface_hook = nullptr;
safetyhook::VmHook *g_updatetexture_hook = nullptr;

void *g_device = nullptr;
bool g_params_logged = false;

// Per-call statistics. Present/BeginScene/EndScene/Reset all fire on the
// main thread only (render threading model, render_path.md) — no atomics.
// `total` drives the one-shot diagnostic burst (first calls of the process
// lifetime, not per report window); `window` drives the 10s pattern reports.
constexpr uint32_t BURST_LOG_CALLS = 3;
constexpr double REPORT_INTERVAL_SEC = 10.0;

struct CallStat {
    uint64_t total = 0;
    uint64_t window = 0;
};

CallStat g_present, g_beginscene, g_endscene, g_reset;
LARGE_INTEGER g_qpc_freq = {};

// ---- Diagnostics ----------------------------------------------------------
// (address description lives in log.cpp: mc2vr::describe_code_address)

void burst_log(const char *what, uint64_t n, const char *extra)
{
    char caller[96];
    describe_code_address(__builtin_return_address(0), caller, sizeof(caller));
    MC2VR_LOG("D3D: %s call #%llu: frame=%llu %s | caller=%s",
              what, (unsigned long long)n,
              (unsigned long long)hooks::frame_count(), extra, caller);
}

// ---- Present-parameter logging (main thread, first Present) ---------------

void log_present_params()
{
    g_params_logged = true;

    void **vt = *(void ***)g_device;
    void *swapchain = nullptr;
    HRESULT hr = ((GetSwapChain_t)vt[SLOT_GetSwapChain])(g_device, 0, &swapchain);
    if (FAILED(hr) || !swapchain) {
        MC2VR_LOG("D3D: GetSwapChain(0) failed hr=%08lx", (unsigned long)hr);
        return;
    }

    D3DPRESENT_PARAMETERS pp = {};
    hr = ((GetPresentParams_t)(*(void ***)swapchain)[SLOT_SC_GetPresentParameters])(swapchain, &pp);

    // Release the AddRef from GetSwapChain through the raw vtable.
    ((HRESULT(__stdcall *)(void *))(*(void ***)swapchain)[SLOT_Release])(swapchain);

    if (FAILED(hr)) {
        MC2VR_LOG("D3D: GetPresentParameters failed hr=%08lx", (unsigned long)hr);
        return;
    }

    view::set_main_rt_size(pp.BackBufferWidth, pp.BackBufferHeight);
    MC2VR_LOG("D3D: present params: %ux%u fmt=%u count=%u windowed=%u swapeffect=%u "
              "refresh=%u interval=0x%08x hdeviceWindow=%p",
              pp.BackBufferWidth, pp.BackBufferHeight, pp.BackBufferFormat,
              pp.BackBufferCount, pp.Windowed, pp.SwapEffect,
              pp.FullScreen_RefreshRateInHz, pp.PresentationInterval,
              pp.hDeviceWindow);
}

// ---- Hook handlers ----------------------------------------------------------

LARGE_INTEGER g_window_start = {};

HRESULT __stdcall present_hook(void *self, const RECT *src, const RECT *dst,
                               HWND hwnd, const RGNDATA *dirty)
{
    g_present.total++;
    g_present.window++;

    if (g_present.total <= BURST_LOG_CALLS) {
        char extra[96];
        _snprintf(extra, sizeof(extra), "args src=%p dst=%p hwnd=%p dirty=%p",
                  src, dst, hwnd, dirty);
        extra[sizeof(extra) - 1] = '\0';
        burst_log("Present", g_present.total, extra);
    }

    if (!g_params_logged) {
        log_present_params(); // main thread — first Present is the safe point
    }

    // 10s call-pattern report — the runtime pinning evidence: Present /
    // EndScene / BeginScene counts must track the FrameTick frame count 1:1.
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_window_start.QuadPart == 0) {
        g_window_start = now;
    } else if ((double)(now.QuadPart - g_window_start.QuadPart) /
                   (double)g_qpc_freq.QuadPart >= REPORT_INTERVAL_SEC) {
        double window_sec =
            (double)(now.QuadPart - g_window_start.QuadPart) / (double)g_qpc_freq.QuadPart;
        MC2VR_LOG("D3D: calls in %.1fs: Present=%llu BeginScene=%llu EndScene=%llu Reset=%llu "
                  "| frames=%llu",
                  window_sec, (unsigned long long)g_present.window,
                  (unsigned long long)g_beginscene.window,
                  (unsigned long long)g_endscene.window,
                  (unsigned long long)g_reset.window,
                  (unsigned long long)hooks::frame_count());

        g_present.window = 0;
        g_beginscene.window = 0;
        g_endscene.window = 0;
        g_reset.window = 0;
        g_window_start = now;
    }

    return g_present_hook->stdcall<HRESULT>(self, src, dst, hwnd, dirty);
}

HRESULT __stdcall beginscene_hook(void *self)
{
    g_beginscene.total++;
    g_beginscene.window++;
    if (g_beginscene.total <= BURST_LOG_CALLS) {
        burst_log("BeginScene", g_beginscene.total, "");
    }
    return g_beginscene_hook->stdcall<HRESULT>(self);
}

HRESULT __stdcall endscene_hook(void *self)
{
    g_endscene.total++;
    g_endscene.window++;
    if (g_endscene.total <= BURST_LOG_CALLS) {
        burst_log("EndScene", g_endscene.total, "");
    }
    return g_endscene_hook->stdcall<HRESULT>(self);
}

HRESULT __stdcall reset_hook(void *self, D3DPRESENT_PARAMETERS *pp)
{
    g_reset.total++;
    g_reset.window++;

    // Reset carries the NEW present parameters — the M4-relevant data. Log
    // every Reset (they're rare).
    if (pp) {
        MC2VR_LOG("D3D: Reset call #%llu: new params %ux%u fmt=%u count=%u windowed=%u "
                  "swapeffect=%u refresh=%u interval=0x%08x | frame=%llu",
                  (unsigned long long)g_reset.total, pp->BackBufferWidth, pp->BackBufferHeight,
                  pp->BackBufferFormat, pp->BackBufferCount, pp->Windowed, pp->SwapEffect,
                  pp->FullScreen_RefreshRateInHz, pp->PresentationInterval,
                  (unsigned long long)hooks::frame_count());
    } else {
        MC2VR_LOG("D3D: Reset call #%llu: NULL params | frame=%llu",
                  (unsigned long long)g_reset.total, (unsigned long long)hooks::frame_count());
    }

    eye::on_reset(); // surfaces are lost; drop the eye RT + main-RT recording
    return g_reset_hook->stdcall<HRESULT>(self, pp);
}

// ---- SetVertexShaderConstantF — the camera channel (view_rewrite.cpp) --------

uint64_t g_setvsconst_calls = 0;

uint64_t g_stretch_calls = 0;

// StretchRect: pass-2 sources pointing at the main RT read the eye RT instead
// (post-effect blits must see pass 2's accumulation, not pass 1's frozen one).
// With eye_monitor_pin=on the EndSubmit RT->backbuffer copy is skipped
// (on_stretch_src sets skip) so the monitor keeps pass 1's LEFT image.
HRESULT __stdcall stretchrect_hook(void *self, void *src, const RECT *src_rect,
                                   void *dst, const RECT *dst_rect, DWORD filter)
{
    g_stretch_calls++;
    bool skip = false;
    src = eye::on_stretch_src(src, dst, &skip);
    if (skip) {
        return S_OK;
    }
    return g_stretchrect_hook->stdcall<HRESULT>(self, src, src_rect, dst, dst_rect,
                                                filter);
}

// UpdateSurface (slot 30): same pass-2 rules as StretchRect (pin skips
// backbuffer writes; main-RT sources read the eye RT instead) — closes the
// "post-effects reading the main RT via other paths" watch item.
HRESULT __stdcall updatesurface_hook(void *self, void *src, void *dst,
                                     const RECT *src_rect, const POINT *dst_pt)
{
    bool skip = false;
    src = eye::on_update_surface_src(src, dst, &skip);
    if (skip) {
        return S_OK;
    }
    return g_updatesurface_hook->stdcall<HRESULT>(self, src, dst, src_rect, dst_pt);
}

// UpdateTexture (slot 31): diagnostic only for now (the source is a texture;
// it cannot be pointer-matched against the main RT surface). Counts pass-2
// activity so the next audit knows whether this path exists at all.
HRESULT __stdcall updatetexture_hook(void *self, void *src, void *dst)
{
    eye::on_update_texture(src, dst);
    return g_updatetexture_hook->stdcall<HRESULT>(self, src, dst);
}

HRESULT __stdcall setrendertarget_hook(void *self, DWORD index, void *surface)
{
    // S2c-2: pass-2 slot-0 sets of the main RT go to the eye RT (device-level
    // substitution only — the game's caller-side RT cache is untouched).
    void *target = eye::on_set_render_target(self, index, surface);
    const HRESULT hr = g_setrt_hook->stdcall<HRESULT>(self, index, target);
    if (index == 0) {
        UINT w = 0, h = 0;
        if (surface) {
            D3DSURFACE_DESC desc = {};
            auto get_desc = (HRESULT(__stdcall *)(void *, D3DSURFACE_DESC *))
                (*(void ***)surface)[SLOT_Surface_GetDesc];
            if (SUCCEEDED(get_desc(surface, &desc))) {
                w = desc.Width;
                h = desc.Height;
            }
        }
        view::on_set_render_target(w, h);
    }
    return hr;
}

HRESULT __stdcall setvsconstf_hook(void *self, UINT start, const float *data, UINT count)
{
    g_setvsconst_calls++;
    if (g_setvsconst_calls <= BURST_LOG_CALLS) {
        char caller[96];
        describe_code_address(__builtin_return_address(0), caller, sizeof(caller));
        MC2VR_LOG("D3D: SetVertexShaderConstantF call #%llu: start=c%u count=%u "
                  "frame=%llu | caller=%s",
                  (unsigned long long)g_setvsconst_calls, (unsigned)start,
                  (unsigned)count, (unsigned long long)hooks::frame_count(), caller);
    }
    const float *out = view::on_set_vs_constant((uint32_t)start, data, (uint32_t)count);
    return g_setvsconstf_hook->stdcall<HRESULT>(self, start, out, count);
}


} // namespace

bool capture_and_hook()
{
    QueryPerformanceFrequency(&g_qpc_freq);

    // Capture: a direct call of the plaintext GetD3DDevice thunk. Returns
    // the live DXVK IDirect3DDevice9 object (proven by M1: it exists at
    // carrier-init time).
    g_device = ((GetD3DDevice_t)MC2_GETD3DDEVICE_THUNK)();
    if (!g_device) {
        MC2VR_LOG("D3D: FATAL — GetD3DDevice() returned NULL at init (unexpected per M1)");
        return false;
    }

    void **vt = *(void ***)g_device;
    char vtable_desc[96];
    describe_code_address(vt[0], vtable_desc, sizeof(vtable_desc));
    MC2VR_LOG("D3D: device captured @ %p (vtable in %s)", g_device, vtable_desc);

    // VmtHook clones the object's vtable and swaps the vptr — DXVK's original
    // vtable is untouched; no DXVK code runs during create() (the swap is a
    // plain aligned pointer store), so this is safe from the init thread.
    auto vmt = safetyhook::VmtHook::create(g_device);
    if (!vmt) {
        MC2VR_LOG("D3D: FATAL — VmtHook create failed (error %u)", (unsigned)vmt.error().type);
        return false;
    }
    g_vmt_hook = new safetyhook::VmtHook(std::move(*vmt)); // leaked by design

    struct SlotSpec {
        size_t slot;
        void *destination;
        safetyhook::VmHook **storage;
        const char *name;
    };

    const SlotSpec slots[] = {
        {SLOT_Present, (void *)&present_hook, &g_present_hook, "Present"},
        {SLOT_BeginScene, (void *)&beginscene_hook, &g_beginscene_hook, "BeginScene"},
        {SLOT_EndScene, (void *)&endscene_hook, &g_endscene_hook, "EndScene"},
        {SLOT_Reset, (void *)&reset_hook, &g_reset_hook, "Reset"},
        {SLOT_SetVertexShaderConstantF, (void *)&setvsconstf_hook, &g_setvsconstf_hook,
         "SetVertexShaderConstantF"},
        {SLOT_SetRenderTarget, (void *)&setrendertarget_hook, &g_setrt_hook,
         "SetRenderTarget"},
        {SLOT_StretchRect, (void *)&stretchrect_hook, &g_stretchrect_hook,
         "StretchRect"},
        {SLOT_UpdateSurface, (void *)&updatesurface_hook, &g_updatesurface_hook,
         "UpdateSurface"},
        {SLOT_UpdateTexture, (void *)&updatetexture_hook, &g_updatetexture_hook,
         "UpdateTexture"},
    };

    for (const SlotSpec &spec : slots) {
        auto hook = g_vmt_hook->hook_method(spec.slot, spec.destination);
        if (!hook) {
            MC2VR_LOG("D3D: FATAL — hook_method(%s, slot %u) failed (error %u)",
                      spec.name, (unsigned)spec.slot, (unsigned)hook.error().type);
            return false;
        }
        *spec.storage = new safetyhook::VmHook(std::move(*hook)); // leaked by design
        MC2VR_LOG("D3D: hooked %s (slot %u) via cloned vtable", spec.name, (unsigned)spec.slot);
    }

    MC2VR_LOG("D3D: VmtHook installed — Present/BeginScene/EndScene/Reset pinned "
              "(M2) + SetVertexShaderConstantF slot %u (the S2 GPU-boundary "
              "channel)", (unsigned)SLOT_SetVertexShaderConstantF);
    return true;
}

} // namespace mc2vr::device
