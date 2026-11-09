#include "hooks/device.hpp"

#include <safetyhook.hpp>

#include <windows.h>
#include <d3d9.h> // types only; nothing here references Direct3DCreate9, so the carrier gains no d3d9.dll import

#include <cstdint>
#include <cstring>

#include "debug/census_hooks.hpp"
#include "debug/gui_census.hpp"
#include "debug/gpu_census.hpp"
#include "debug/ps_census.hpp"
#include "game/addresses.h"
#include "hooks/frame_tick.hpp"
#include "init.hpp"
#include "log.hpp"
#include "vr/adaptation.hpp"
#include "vr/device_params.hpp"
#include "vr/eye_replay.hpp"
#include "vr/hud_timing.hpp"
#include "vr/msaa.hpp"
#include "vr/render_outputs.hpp"
#include "vr/render_size.hpp"
#include "vr/view_rewrite.hpp"

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
constexpr size_t SLOT_TestCooperativeLevel = 3;
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
constexpr size_t SLOT_SetDepthStencilSurface = 39;
constexpr size_t SLOT_SetTexture = 65;
// IDirect3D9::CreateDevice (d3d9.h method order, cross-checked against the
// game's observed use: RenderSystem_Init reads the adapter count through
// IDirect3D9 vtable +0x10 = slot 4 GetAdapterCount).
constexpr size_t SLOT_D3D9_CreateDevice = 16;

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
safetyhook::VmHook *g_setds_hook = nullptr;
safetyhook::VmHook *g_settex_hook = nullptr;
IDirect3DDevice9 *g_device = nullptr;
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

SafetyHookInline g_d3dcreate9_hook;          // game's IAT thunk, installed at attach
safetyhook::VmtHook *g_d3d9_vmt_hook = nullptr;  // IDirect3D9, leaked by design
safetyhook::VmHook *g_createdevice_hook = nullptr; // leaked by design

bool hook_device(IDirect3DDevice9 *device);

// IDirect3D9::CreateDevice — the device's birth. Runs on the engine's main
// thread inside RenderSystem_CreateDevice (itself inside the first
// SetupRenderTargets — docs/reverse_engineering/render_target_table.md). The
// device VmtHook is installed HERE, before CreateDevice returns, so no engine
// device call (incl. the setup tail's GetRenderTarget/GetDepthStencilSurface)
// precedes our hooks. Replaces the old polling capture, which raced the main
// thread (live 2026-11-08: hooks landed while SetupRenderTargets was running).
HRESULT __stdcall createdevice_hook(IDirect3D9 *self, UINT adapter, D3DDEVTYPE device_type,
                                     HWND focus_window, DWORD behavior_flags,
                                     D3DPRESENT_PARAMETERS *pp,
                                     IDirect3DDevice9 **out_device)
{
    D3DPRESENT_PARAMETERS used = {};
    if (pp != nullptr) {
        used = device_params::apply_param_policy(*pp, "CreateDevice");
    }
    behavior_flags = device_params::apply_behavior_policy(behavior_flags);
    const HRESULT hr = g_createdevice_hook->stdcall<HRESULT>(self, adapter, device_type,
                                                             focus_window, behavior_flags,
                                                             pp ? &used : nullptr, out_device);
    if (pp != nullptr) {
        device_params::copy_back_params(pp, used);
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
IDirect3D9 *__stdcall d3dcreate9_hook(UINT sdk_version)
{
    IDirect3D9 *d3d = g_d3dcreate9_hook.stdcall<IDirect3D9 *>(sdk_version);
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

// ---- Hook handlers ----------------------------------------------------------

LARGE_INTEGER g_window_start = {};

HRESULT __stdcall present_hook(IDirect3DDevice9 *self, const RECT *src, const RECT *dst,
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
        g_params_logged = true;
        device_params::log_present_params(g_device); // main thread — first Present is the safe point
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

HRESULT __stdcall beginscene_hook(IDirect3DDevice9 *self)
{
    g_beginscene.total++;
    g_beginscene.window++;
    if (g_beginscene.total <= BURST_LOG_CALLS) {
        burst_log("BeginScene", g_beginscene.total, "");
    }
    return g_beginscene_hook->stdcall<HRESULT>(self);
}

HRESULT __stdcall endscene_hook(IDirect3DDevice9 *self)
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
HRESULT __stdcall tcl_hook(IDirect3DDevice9 *self)
{
    if (render_size::consume_forced_reset(__builtin_return_address(0))) {
        return D3DERR_DEVICENOTRESET;
    }
    return g_tcl_hook->stdcall<HRESULT>(self);
}

HRESULT __stdcall reset_hook(IDirect3DDevice9 *self, D3DPRESENT_PARAMETERS *pp)
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
    gpu_census::on_reset();
    ps_census::on_reset();

    // A Reset carries NEW present params (resolution etc.) — the same policy
    // as CreateDevice (vsync unlock, MSAA move), on a copy.
    D3DPRESENT_PARAMETERS used = {};
    if (pp != nullptr) {
        used = device_params::apply_param_policy(*pp, "Reset");
    }

    render_outputs::on_reset(true, 0);
    const HRESULT hr = g_reset_hook->stdcall<HRESULT>(self, pp ? &used : nullptr);
    if (pp != nullptr) {
        device_params::copy_back_params(pp, used);
    }
    render_outputs::on_reset(false, (long)hr);
    msaa::on_reset_done();
    if (SUCCEEDED(hr)) {
        adaptation::on_reset_done();
    }
    return hr;
}

// ---- SetVertexShaderConstantF — the camera channel (view_rewrite.cpp) --------

uint64_t g_setvsconst_calls = 0;

uint64_t g_stretch_calls = 0;

// StretchRect: scene MSAA resolves before reads; the hook also provides the
// original-method trampoline for the carrier's own blits (blit_surfaces).
HRESULT __stdcall stretchrect_hook(IDirect3DDevice9 *self, IDirect3DSurface9 *src,
                                   const RECT *src_rect, IDirect3DSurface9 *dst,
                                   const RECT *dst_rect, D3DTEXTUREFILTERTYPE filter)
{
    g_stretch_calls++;
    msaa::before_stretch_rect(src, dst);  // resolves an MSAA stand-in read as the source
    return g_stretchrect_hook->stdcall<HRESULT>(self, src, src_rect, dst, dst_rect,
                                                filter);
}

HRESULT __stdcall setrendertarget_hook(IDirect3DDevice9 *self, DWORD index,
                                       IDirect3DSurface9 *surface)
{
    // The pass gate for the view rewrite; scene MSAA swaps in its stand-ins
    // (the engine only ever sees its own surfaces).
    gui_census::on_set_render_target(index, surface);
    const HRESULT hr = g_setrt_hook->stdcall<HRESULT>(self, index, msaa::map_render_target(index, surface));
    IDirect3DSurface9 *depth = nullptr;
    if (index == 0 && msaa::depth_after_rt0(&depth)) {
        g_setds_hook->stdcall<HRESULT>(self, depth);  // the depth that fits the new RT0
    }
    if (index == 0) {
        UINT w = 0, h = 0;
        if (surface) {
            D3DSURFACE_DESC desc = {};
            if (SUCCEEDED(surface->GetDesc(&desc))) {
                w = desc.Width;
                h = desc.Height;
            }
        }
        view_rewrite::on_set_render_target(w, h);
        gpu_census::on_set_rt(w, h);
        ps_census::on_set_rt(w, h);
    }
    return hr;
}

// SetDepthStencilSurface / SetTexture: scene MSAA's depth mapping and
// resolve-before-sampling (vr/msaa.hpp).
HRESULT __stdcall setds_hook(IDirect3DDevice9 *self, IDirect3DSurface9 *surface)
{
    gui_census::on_set_depth(surface);
    return g_setds_hook->stdcall<HRESULT>(self, msaa::map_depth(surface));
}

HRESULT __stdcall settex_hook(IDirect3DDevice9 *self, DWORD stage, IDirect3DBaseTexture9 *texture)
{
    msaa::before_set_texture(stage, texture);
    return g_settex_hook->stdcall<HRESULT>(self, stage, texture);
}

HRESULT __stdcall setvsconstf_hook(IDirect3DDevice9 *self, UINT start, const float *data, UINT count)
{
    g_setvsconst_calls++;
    gui_census::on_shader_constants(false, start, data, count);
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

} // namespace

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
bool hook_device(IDirect3DDevice9 *device)
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
        {SLOT_SetDepthStencilSurface, (void *)&setds_hook, &g_setds_hook, "SetDepthStencilSurface"},
        {SLOT_SetTexture, (void *)&settex_hook, &g_settex_hook, "SetTexture"},
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

    // Debug-only slots (observe + forward; each a no-op unless its tool is on).
    census_hooks::install(g_device); // debug_ps_census / debug_gpu_census / debug_gui_census

    MC2VR_LOG("D3D: VmtHook installed at device creation — Present/BeginScene/EndScene/"
              "Reset pinned + SetVertexShaderConstantF slot %u (the GPU-boundary channel)",
              (unsigned)SLOT_SetVertexShaderConstantF);
    return true;
}

} // namespace

safetyhook::VmHook *hook_method(size_t slot, void *destination, unsigned *error)
{
    if (g_vmt_hook == nullptr) {
        if (error) *error = 0;
        return nullptr;
    }
    auto hook = g_vmt_hook->hook_method(slot, destination);
    if (!hook) {
        if (error) *error = (unsigned)hook.error().type;
        return nullptr;
    }
    return new safetyhook::VmHook(std::move(*hook)); // leaked by design
}

bool stretch_surfaces(IDirect3DSurface9 *src, const RECT *src_rect, IDirect3DSurface9 *dst,
                      const RECT *dst_rect, uint32_t filter)
{
    if (!g_device || !g_stretchrect_hook || !src || !dst) {
        return false;
    }
    // Through the trampoline (no re-entry into the pass-2 redirect logic).
    return SUCCEEDED(g_stretchrect_hook->stdcall<HRESULT>(g_device, src, src_rect, dst,
                                                          dst_rect, (D3DTEXTUREFILTERTYPE)filter));
}

bool blit_surfaces(IDirect3DSurface9 *src, IDirect3DSurface9 *dst)
{
    if (!g_device || !g_stretchrect_hook || !src || !dst) {
        return false;
    }
    // Full-surface copy, no filtering (both surfaces are the same size).
    // Through the trampoline, NOT the hooked vtable: the carrier's own
    // blits must not re-enter the device hooks (scene MSAA's mapping, the censuses).
    const HRESULT hr = g_stretchrect_hook->stdcall<HRESULT>(
        g_device, src, nullptr, dst, nullptr, D3DTEXF_NONE);
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
