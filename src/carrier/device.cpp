#include "device.hpp"

#include <safetyhook.hpp>

#include <windows.h>
#include <d3d9.h> // types only; nothing here references Direct3DCreate9, so the carrier gains no d3d9.dll import

#include <cstdint>
#include <cstring>

#include "game_addresses.h"
#include "eye_replay.hpp"
#include "hooks.hpp"
#include "hud_timing.hpp"
#include "init.hpp"
#include "render_outputs.hpp"
#include "render_size.hpp"
#include "adaptation.hpp"
#include "log.hpp"
#include "view_rewrite.hpp"
#include "debug/rt_probe.hpp"

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
constexpr size_t SLOT_TestCooperativeLevel = 3;
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
constexpr size_t SLOT_Surface_GetDesc = 12;
// rt_probe (debug_rt_probe=on only): creation + getter census slots.
constexpr size_t SLOT_GetBackBuffer = 18;
constexpr size_t SLOT_CreateTexture = 23;
constexpr size_t SLOT_CreateRenderTarget = 28;
constexpr size_t SLOT_CreateDepthStencilSurface = 29;
constexpr size_t SLOT_CreateOffscreenPlainSurface = 36;
constexpr size_t SLOT_GetRenderTarget = 38;
constexpr size_t SLOT_GetDepthStencilSurface = 40;
// IDirect3DSwapChain9::GetPresentParameters
constexpr size_t SLOT_SC_GetPresentParameters = 9;
// IDirect3D9::CreateDevice (d3d9.h method order, cross-checked against the
// game's observed use: RenderSystem_Init reads the adapter count through
// IDirect3D9 vtable +0x10 = slot 4 GetAdapterCount).
constexpr size_t SLOT_D3D9_CreateDevice = 16;

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
using SetVertexShaderConstantF_t = HRESULT(__stdcall *)(void *, UINT, const float *, UINT);

// Leaked by design (see device.hpp).
safetyhook::VmtHook *g_vmt_hook = nullptr;
safetyhook::VmHook *g_present_hook = nullptr;
safetyhook::VmHook *g_beginscene_hook = nullptr;
safetyhook::VmHook *g_endscene_hook = nullptr;
safetyhook::VmHook *g_reset_hook = nullptr;
safetyhook::VmHook *g_tcl_hook = nullptr;
safetyhook::VmHook *g_setvsconstf_hook = nullptr;
safetyhook::VmHook *g_setrt_hook = nullptr;
safetyhook::VmHook *g_stretchrect_hook = nullptr;
// rt_probe census hooks (installed only with debug_rt_probe=on).
safetyhook::VmHook *g_getswapchain_hook = nullptr;
safetyhook::VmHook *g_getbackbuffer_hook = nullptr;
safetyhook::VmHook *g_createtexture_hook = nullptr;
safetyhook::VmHook *g_creatert_hook = nullptr;
safetyhook::VmHook *g_createds_hook = nullptr;
safetyhook::VmHook *g_createplain_hook = nullptr;
safetyhook::VmHook *g_getrt_hook = nullptr;
safetyhook::VmHook *g_getds_hook = nullptr;

void *g_device = nullptr;
bool g_params_logged = false;

// Per-call statistics. Present/BeginScene/EndScene/Reset all fire on the
// main thread only (render threading model, docs/reverse_engineering/render_threading.md) — no atomics.
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

// ---- Pacing: vsync=off (CreateDevice/Reset interval patch) ----------
// The frame's two Presents (present-prev at each BeginSubmit) each block on
// a 60 Hz vsync slot -> ~30 Hz game (live-log-proven). vsync=off forces
// D3DPRESENT_INTERVAL_IMMEDIATE so the game renders as fast as the passes
// actually cost; the OpenXR host/runtime paces the HMD.
bool g_present_immediate = false;
SafetyHookInline g_d3dcreate9_hook;          // game's IAT thunk, installed at attach
safetyhook::VmtHook *g_d3d9_vmt_hook = nullptr;  // IDirect3D9, leaked by design
safetyhook::VmHook *g_createdevice_hook = nullptr; // leaked by design

bool hook_device(void *device);

// ---- Device-parameter policy ----------------------------------------------------
// Applied to a COPY of the engine's D3DPRESENT_PARAMETERS: the engine keeps
// its stored copy exactly as it asked (RenderSystem_ApplySettings compares
// that stored copy field by field — MultiSampleType @+0x10, interval @+0x34 —
// against its settings, so in-place patches read as "settings changed").
//   - vsync=off: PresentationInterval = IMMEDIATE.
//   - The swapchain is only the carrier's monitor mirror, so it runs without
//     MSAA; the engine-requested MSAA moves to the carrier's front/depth
//     surfaces (render_outputs).
D3DPRESENT_PARAMETERS apply_param_policy(const D3DPRESENT_PARAMETERS &engine, const char *what)
{
    D3DPRESENT_PARAMETERS p = engine;
    if (g_present_immediate && p.PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE) {
        MC2VR_LOG("vsync: %s params: %ux%u windowed=%u swapeffect=%u refresh=%u "
                  "interval=0x%08x -> forcing IMMEDIATE",
                  what, p.BackBufferWidth, p.BackBufferHeight, p.Windowed, p.SwapEffect,
                  p.FullScreen_RefreshRateInHz, p.PresentationInterval);
        p.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    }
    render_outputs::note_requested_msaa((uint32_t)p.MultiSampleType, p.MultiSampleQuality);
    if (p.MultiSampleType != D3DMULTISAMPLE_NONE) {
        MC2VR_LOG("outputs: %s swapchain MSAA %u/%lu -> none (moved to the carrier front)",
                  what, (unsigned)p.MultiSampleType, (unsigned long)p.MultiSampleQuality);
        p.MultiSampleType = D3DMULTISAMPLE_NONE;
        p.MultiSampleQuality = 0;
    }
    return p;
}

// D3D may update the params it was given (BackBufferCount/Format, …): mirror
// those back, keeping the policy fields exactly as the engine requested.
void copy_back_params(D3DPRESENT_PARAMETERS *engine, const D3DPRESENT_PARAMETERS &used)
{
    const D3DMULTISAMPLE_TYPE ms = engine->MultiSampleType;
    const DWORD msq = engine->MultiSampleQuality;
    const UINT interval = engine->PresentationInterval;
    *engine = used;
    engine->MultiSampleType = ms;
    engine->MultiSampleQuality = msq;
    engine->PresentationInterval = interval;
}

// IDirect3D9::CreateDevice — the device's birth. Runs on the engine's main
// thread inside RenderSystem_CreateDevice (itself inside the first
// SetupRenderTargets — docs/reverse_engineering/render_target_table.md). The
// device VmtHook is installed HERE, before CreateDevice returns, so no engine
// device call (incl. the setup tail's GetRenderTarget/GetDepthStencilSurface)
// precedes our hooks. Replaces the old polling capture, which raced the main
// thread (live 2026-11-08: hooks landed while SetupRenderTargets was running).
HRESULT __stdcall createdevice_hook(void *self, UINT adapter, DWORD device_type,
                                     HWND focus_window, DWORD behavior_flags,
                                     D3DPRESENT_PARAMETERS *pp, void **out_device)
{
    D3DPRESENT_PARAMETERS used = {};
    if (pp != nullptr) {
        used = apply_param_policy(*pp, "CreateDevice");
    }
    const HRESULT hr = g_createdevice_hook->stdcall<HRESULT>(self, adapter, device_type,
                                                             focus_window, behavior_flags,
                                                             pp ? &used : nullptr, out_device);
    if (pp != nullptr) {
        copy_back_params(pp, used);
    }
    if (SUCCEEDED(hr) && out_device != nullptr && *out_device != nullptr) {
        hook_device(*out_device);
    } else {
        MC2VR_LOG("D3D: CreateDevice failed hr=%08lx — no device hooks", (unsigned long)hr);
    }
    return hr;
}

// The game's Direct3DCreate9 thunk (jmp [0x00b05620]). The original is
// reached through the trampoline — the carrier still gains no d3d9.dll
// import. Main thread, called exactly once (RenderSystem_Init guards on
// g_D3D9 NULL); the fresh IDirect3D9 is unshared at this point, so the VmtHook
// (vptr swap, aligned store) is safe without thread suspension.
void *__stdcall d3dcreate9_hook(UINT sdk_version)
{
    void *d3d = g_d3dcreate9_hook.stdcall<void *>(sdk_version);
    if (d3d == nullptr || g_d3d9_vmt_hook != nullptr) {
        return d3d;
    }

    auto vmt = safetyhook::VmtHook::create(d3d);
    if (!vmt) {
        MC2VR_LOG("D3D: FATAL — IDirect3D9 VmtHook create failed (error %u) — "
                  "no device hooks this run",
                  (unsigned)vmt.error().type);
        return d3d;
    }
    g_d3d9_vmt_hook = new safetyhook::VmtHook(std::move(*vmt)); // leaked by design

    auto hook = g_d3d9_vmt_hook->hook_method(SLOT_D3D9_CreateDevice,
                                             (void *)&createdevice_hook);
    if (!hook) {
        MC2VR_LOG("D3D: FATAL — hook_method(CreateDevice, slot %u) failed (error %u) — "
                  "no device hooks this run",
                  (unsigned)SLOT_D3D9_CreateDevice, (unsigned)hook.error().type);
        return d3d;
    }
    g_createdevice_hook = new safetyhook::VmHook(std::move(*hook)); // leaked by design
    MC2VR_LOG("D3D: IDirect3D9 captured @ %p, CreateDevice hooked (slot %u)",
              d3d, (unsigned)SLOT_D3D9_CreateDevice);
    return d3d;
}

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
    rt_probe::on_present(self);

    if (g_present.total <= BURST_LOG_CALLS) {
        char extra[96];
        _snprintf(extra, sizeof(extra), "args src=%p dst=%p hwnd=%p dirty=%p",
                  src, dst, hwnd, dirty);
        extra[sizeof(extra) - 1] = '\0';
        burst_log("Present", g_present.total, extra);
    }

    if (!g_params_logged) {
        log_present_params(); // main thread — first Present is the safe point
        // The engine is past boot (device built, render shell constructed,
        // SecuROM loader done): run the installs that need a running engine.
        static bool engine_running = false;
        if (!engine_running) {
            engine_running = true;
            on_engine_running();
        }
    }

    // HUD timing: Present is a per-frame phase sample point.
    hud_timing::on_present();

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

    // Monitor mirror: the engine no longer draws into the swapchain —
    // compose the latest LEFT image into it before presenting.
    render_size::on_present();
    render_outputs::on_present(self);

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

// TestCooperativeLevel: forwarded, except that a render-size switch
// (render_size.cpp) answers RenderSystem_CheckDevice's per-frame call ONCE
// with D3DERR_DEVICENOTRESET — the engine then runs its own device-lost
// recovery (release -> Reset -> OnScreenResize -> restore) at the new size.
HRESULT __stdcall tcl_hook(void *self)
{
    if (render_size::consume_forced_reset(__builtin_return_address(0))) {
        return D3DERR_DEVICENOTRESET;
    }
    return g_tcl_hook->stdcall<HRESULT>(self);
}

HRESULT __stdcall reset_hook(void *self, D3DPRESENT_PARAMETERS *pp)
{
    g_reset.total++;
    g_reset.window++;

    // Reset carries the NEW present parameters. Log
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

    eye_replay::on_reset(); // the shared eye ring is dropped (re-created lazily)

    // A Reset carries NEW present params (resolution etc.) — the same policy
    // as CreateDevice (vsync unlock, MSAA move), on a copy.
    D3DPRESENT_PARAMETERS used = {};
    if (pp != nullptr) {
        used = apply_param_policy(*pp, "Reset");
    }

    rt_probe::on_reset(true, 0);
    render_outputs::on_reset(true, 0);
    const HRESULT hr = g_reset_hook->stdcall<HRESULT>(self, pp ? &used : nullptr);
    if (pp != nullptr) {
        copy_back_params(pp, used);
    }
    render_outputs::on_reset(false, (long)hr);
    if (SUCCEEDED(hr)) {
        adaptation::on_reset_done();
    }
    rt_probe::on_reset(false, (long)hr);
    return hr;
}

// ---- SetVertexShaderConstantF — the camera channel (view_rewrite.cpp) --------

uint64_t g_setvsconst_calls = 0;

uint64_t g_stretch_calls = 0;

// StretchRect: observed (rt_probe) and forwarded; the hook also provides the
// original-method trampoline for the carrier's own blits (blit_surfaces).
HRESULT __stdcall stretchrect_hook(void *self, void *src, const RECT *src_rect,
                                   void *dst, const RECT *dst_rect, DWORD filter)
{
    g_stretch_calls++;
    rt_probe::on_stretch_rect(src, dst, __builtin_return_address(0));
    return g_stretchrect_hook->stdcall<HRESULT>(self, src, src_rect, dst, dst_rect,
                                                filter);
}

HRESULT __stdcall setrendertarget_hook(void *self, DWORD index, void *surface)
{
    // Observed, never altered: the pass gate for the view rewrite.
    rt_probe::on_set_render_target(index, surface, __builtin_return_address(0));
    const HRESULT hr = g_setrt_hook->stdcall<HRESULT>(self, index, surface);
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
        view_rewrite::on_set_render_target(w, h);
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
    const float *out = view_rewrite::on_set_vs_constant((uint32_t)start, data, (uint32_t)count);
    return g_setvsconstf_hook->stdcall<HRESULT>(self, start, out, count);
}

// ---- rt_probe census (debug_rt_probe=on; observe + forward only) -------------

HRESULT __stdcall createtexture_hook(void *self, UINT w, UINT h, UINT levels, DWORD usage,
                                     D3DFORMAT fmt, D3DPOOL pool, void **out, HANDLE *shared)
{
    const HRESULT hr = g_createtexture_hook->stdcall<HRESULT>(self, w, h, levels, usage, fmt,
                                                              pool, out, shared);
    rt_probe::on_create(rt_probe::Create::Texture, w, h, (uint32_t)fmt, usage, (uint32_t)pool,
                        (long)hr, out ? *out : nullptr, __builtin_return_address(0));
    return hr;
}

HRESULT __stdcall creatert_hook(void *self, UINT w, UINT h, D3DFORMAT fmt,
                                D3DMULTISAMPLE_TYPE ms, DWORD msq, BOOL lockable, void **out,
                                HANDLE *shared)
{
    const HRESULT hr = g_creatert_hook->stdcall<HRESULT>(self, w, h, fmt, ms, msq, lockable, out,
                                                         shared);
    rt_probe::on_create(rt_probe::Create::RenderTarget, w, h, (uint32_t)fmt,
                        D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, (long)hr, out ? *out : nullptr,
                        __builtin_return_address(0));
    return hr;
}

HRESULT __stdcall createds_hook(void *self, UINT w, UINT h, D3DFORMAT fmt,
                                D3DMULTISAMPLE_TYPE ms, DWORD msq, BOOL discard, void **out,
                                HANDLE *shared)
{
    const HRESULT hr = g_createds_hook->stdcall<HRESULT>(self, w, h, fmt, ms, msq, discard, out,
                                                         shared);
    rt_probe::on_create(rt_probe::Create::DepthStencil, w, h, (uint32_t)fmt,
                        D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, (long)hr, out ? *out : nullptr,
                        __builtin_return_address(0));
    return hr;
}

HRESULT __stdcall createplain_hook(void *self, UINT w, UINT h, D3DFORMAT fmt, D3DPOOL pool,
                                   void **out, HANDLE *shared)
{
    const HRESULT hr = g_createplain_hook->stdcall<HRESULT>(self, w, h, fmt, pool, out, shared);
    rt_probe::on_create(rt_probe::Create::OffscreenPlain, w, h, (uint32_t)fmt, 0,
                        (uint32_t)pool, (long)hr, out ? *out : nullptr,
                        __builtin_return_address(0));
    return hr;
}

HRESULT __stdcall getswapchain_hook(void *self, UINT index, void **out)
{
    const HRESULT hr = g_getswapchain_hook->stdcall<HRESULT>(self, index, out);
    rt_probe::on_getter(rt_probe::Getter::SwapChain, index,
                        SUCCEEDED(hr) && out ? *out : nullptr, __builtin_return_address(0));
    return hr;
}

HRESULT __stdcall getbackbuffer_hook(void *self, UINT swapchain, UINT index,
                                     D3DBACKBUFFER_TYPE type, void **out)
{
    const HRESULT hr = g_getbackbuffer_hook->stdcall<HRESULT>(self, swapchain, index, type, out);
    rt_probe::on_getter(rt_probe::Getter::BackBuffer, index,
                        SUCCEEDED(hr) && out ? *out : nullptr, __builtin_return_address(0));
    return hr;
}

HRESULT __stdcall getrt_hook(void *self, DWORD index, void **out)
{
    const HRESULT hr = g_getrt_hook->stdcall<HRESULT>(self, index, out);
    rt_probe::on_getter(rt_probe::Getter::RenderTarget, index,
                        SUCCEEDED(hr) && out ? *out : nullptr, __builtin_return_address(0));
    return hr;
}

HRESULT __stdcall getds_hook(void *self, void **out)
{
    const HRESULT hr = g_getds_hook->stdcall<HRESULT>(self, out);
    rt_probe::on_getter(rt_probe::Getter::DepthStencil, 0, SUCCEEDED(hr) && out ? *out : nullptr,
                        __builtin_return_address(0));
    return hr;
}

} // namespace

bool set_vsync(const char *value)
{
    if (strcmp(value, "on") == 0) {
        g_present_immediate = false;
    } else if (strcmp(value, "off") == 0) {
        g_present_immediate = true;
    } else {
        return false;
    }
    MC2VR_LOG("vsync: vsync=%s (%s)", value,
              g_present_immediate
                  ? "CreateDevice/Reset force D3DPRESENT_INTERVAL_IMMEDIATE"
                  : "game's own present params untouched");
    return true;
}

bool install_d3d9_gate()
{
    if (g_d3dcreate9_hook) {
        return true; // already installed
    }

    // Trap-based install while the game is still suspended (attach); the
    // 6-byte `jmp dword ptr [IAT]` thunk is plaintext .text well below the
    // SecuROM region. Root of the device-hook chain (Direct3DCreate9 ->
    // CreateDevice -> device VmtHook): failing it means no device hooks this
    // run — the game still runs unmodified.
    auto result = SafetyHookInline::create(
        reinterpret_cast<uint8_t *>(MC2_D3DCREATE9_THUNK),
        reinterpret_cast<uint8_t *>(&d3dcreate9_hook));
    if (!result) {
        MC2VR_LOG("D3D: FATAL — Direct3DCreate9 thunk hook install failed @ %p "
                  "(error %u) — no device hooks this run",
                  (void *)MC2_D3DCREATE9_THUNK, (unsigned)result.error().type);
        return false;
    }
    g_d3dcreate9_hook = std::move(*result);
    MC2VR_LOG("D3D: installed Direct3DCreate9 thunk hook @ %p",
              (void *)MC2_D3DCREATE9_THUNK);
    return true;
}

namespace {

// Called from createdevice_hook (main thread; the engine has not seen the
// device yet). A second CreateDevice (device re-creation) re-hooks the new
// object; the trampolines stay valid — every device shares DXVK's vtable.
bool hook_device(void *device)
{
    QueryPerformanceFrequency(&g_qpc_freq);

    if (g_device != nullptr && g_device != device) {
        MC2VR_LOG("D3D: NEW device %p replaces %p — re-hooking", device, g_device);
    }
    g_device = device;
    g_params_logged = false;

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
        {SLOT_TestCooperativeLevel, (void *)&tcl_hook, &g_tcl_hook, "TestCooperativeLevel"},
        {SLOT_SetVertexShaderConstantF, (void *)&setvsconstf_hook, &g_setvsconstf_hook,
         "SetVertexShaderConstantF"},
        {SLOT_SetRenderTarget, (void *)&setrendertarget_hook, &g_setrt_hook,
         "SetRenderTarget"},
        {SLOT_StretchRect, (void *)&stretchrect_hook, &g_stretchrect_hook,
         "StretchRect"},
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

    if (rt_probe::enabled()) {
        const SlotSpec probe_slots[] = {
            {SLOT_GetSwapChain, (void *)&getswapchain_hook, &g_getswapchain_hook, "GetSwapChain"},
            {SLOT_GetBackBuffer, (void *)&getbackbuffer_hook, &g_getbackbuffer_hook,
             "GetBackBuffer"},
            {SLOT_CreateTexture, (void *)&createtexture_hook, &g_createtexture_hook,
             "CreateTexture"},
            {SLOT_CreateRenderTarget, (void *)&creatert_hook, &g_creatert_hook,
             "CreateRenderTarget"},
            {SLOT_CreateDepthStencilSurface, (void *)&createds_hook, &g_createds_hook,
             "CreateDepthStencilSurface"},
            {SLOT_CreateOffscreenPlainSurface, (void *)&createplain_hook, &g_createplain_hook,
             "CreateOffscreenPlainSurface"},
            {SLOT_GetRenderTarget, (void *)&getrt_hook, &g_getrt_hook, "GetRenderTarget"},
            {SLOT_GetDepthStencilSurface, (void *)&getds_hook, &g_getds_hook,
             "GetDepthStencilSurface"},
        };
        for (const SlotSpec &spec : probe_slots) {
            auto hook = g_vmt_hook->hook_method(spec.slot, spec.destination);
            if (!hook) {
                // Non-fatal: the probe only loses this tap.
                MC2VR_LOG("rtprobe: hook_method(%s, slot %u) failed (error %u)", spec.name,
                          (unsigned)spec.slot, (unsigned)hook.error().type);
                continue;
            }
            *spec.storage = new safetyhook::VmHook(std::move(*hook)); // leaked by design
            MC2VR_LOG("rtprobe: hooked %s (slot %u)", spec.name, (unsigned)spec.slot);
        }
    }

    MC2VR_LOG("D3D: VmtHook installed at device creation — Present/BeginScene/EndScene/"
              "Reset pinned + SetVertexShaderConstantF slot %u (the GPU-boundary channel)",
              (unsigned)SLOT_SetVertexShaderConstantF);
    return true;
}

} // namespace

bool stretch_surfaces(void *src, const RECT *src_rect, void *dst, const RECT *dst_rect,
                      uint32_t filter)
{
    if (!g_device || !g_stretchrect_hook || !src || !dst) {
        return false;
    }
    // Through the trampoline (no re-entry into the pass-2 redirect logic).
    return SUCCEEDED(g_stretchrect_hook->stdcall<HRESULT>(g_device, src, src_rect, dst,
                                                          dst_rect, (DWORD)filter));
}

bool blit_surfaces(void *src, void *dst)
{
    if (!g_device || !g_stretchrect_hook || !src || !dst) {
        return false;
    }
    // Full-surface copy, no filtering (both surfaces are the same size).
    // Through the trampoline, NOT the hooked vtable: the carrier's own
    // blits must not re-enter the device hooks (rt_probe classification).
    const HRESULT hr = g_stretchrect_hook->stdcall<HRESULT>(
        g_device, src, nullptr, dst, nullptr, 0 /* D3DTEXF_NONE */);
    if (FAILED(hr)) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            MC2VR_LOG("D3D: blit_surfaces StretchRect FAILED hr=%08lx "
                      "(carrier blits degraded this run)", (unsigned long)hr);
        }
        return false;
    }
    return true;
}

} // namespace mc2vr::device
