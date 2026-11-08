// Carrier-owned frame outputs. See render_outputs.hpp.

#include "render_outputs.hpp"

#include <safetyhook.hpp>

#include <windows.h>
#include <d3d9.h> // types only

#include <cstdint>
#include <cstring>

#include "device.hpp"
#include "game_addresses.h"
#include "hooks.hpp"
#include "log.hpp"
#include "debug/rt_probe.hpp"
#include "render_size.hpp"

namespace mc2vr::render_outputs {

namespace {

// Device / surface vtable slots (d3d9.h order).
constexpr size_t DSLOT_GetBackBuffer = 18;
constexpr size_t DSLOT_CreateRenderTarget = 28;
constexpr size_t DSLOT_CreateDepthStencilSurface = 29;
constexpr size_t DSLOT_ColorFill = 35;
constexpr size_t SSLOT_AddRef = 1;
constexpr size_t SSLOT_Release = 2;
constexpr size_t SSLOT_GetDesc = 12;

using CreateSurf_t = HRESULT(__stdcall *)(void *, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE,
                                          DWORD, BOOL, void **, HANDLE *);
using GetBackBuffer_t = HRESULT(__stdcall *)(void *, UINT, UINT, D3DBACKBUFFER_TYPE, void **);
using ColorFill_t = HRESULT(__stdcall *)(void *, void *, const RECT *, D3DCOLOR);
using Ref_t = ULONG(__stdcall *)(void *);
using GetDesc_t = HRESULT(__stdcall *)(void *, D3DSURFACE_DESC *);

// Carrier surfaces (one ref each held by us; engine slots hold their own).
void *g_front = nullptr;  // RT, engine front desc (+ requested MSAA)
void *g_depth = nullptr;  // DS, engine auto-depth desc (+ requested MSAA)
void *g_mirror = nullptr; // non-MSAA RT, front size: the LEFT monitor image
void *g_scratch = nullptr; // non-MSAA RT, front size: on-demand front resolve (dumps)
// Pointers we released at Reset; engine slots still holding them must be
// overwritten without a Release (they are dead).
void *g_retired_front = nullptr;
void *g_retired_depth = nullptr;

void *g_real_bb = nullptr; // swapchain backbuffer (identity only, no ref held)
bool g_have_msaa = false;
D3DMULTISAMPLE_TYPE g_msaa = D3DMULTISAMPLE_NONE;
DWORD g_msaa_q = 0;

uint64_t g_mirror_frame = 0; // frame of the last 1->2 mirror refresh
bool g_mirror_valid = false;

// Window counters (main thread writes; poller reads approximately).
uint64_t g_takeovers = 0, g_monitor_blits = 0, g_mirror_updates = 0, g_failures = 0;
uint64_t g_present_skips = 0;
// Transition frame after a device Reset: its HDR adaptation step runs on the
// freshly re-created (empty) luminance chain and comes out fully exposed — a
// single white frame (live 2026-11-08). It is not shown: no eye capture (the
// host keeps presenting the previous pair, reprojected) and a black monitor
// frame. Armed at Reset END, cleared at the end of that frame's pass 2.
bool g_transition_frame = false;
uint64_t g_transition_frames = 0;
bool g_skip_armed = false; // we set g_SuppressPresent for the current pass 1

SafetyHookInline g_setup_hook;
SafetyHookMid g_restore_mid;

// ---- engine holders -------------------------------------------------------------

void **slot(uintptr_t off)
{
    return (void **)(MC2_G_RENDERSHELL + off);
}
void **auto_depth_ref()
{
    return (void **)MC2_G_DX9_AUTODEPTH_REF;
}

void *engine_device()
{
    const uintptr_t lti = *(volatile uintptr_t *)MC2_G_LTIRENDERER;
    return lti ? *(void **)(lti + MC2_LTIRENDERER_DX9STATE_OFF) : nullptr;
}

void add_ref(void *p)
{
    ((Ref_t)(*(void ***)p)[SSLOT_AddRef])(p);
}
void release(void *p)
{
    ((Ref_t)(*(void ***)p)[SSLOT_Release])(p);
}
bool get_desc(void *p, D3DSURFACE_DESC *d)
{
    return p && SUCCEEDED(((GetDesc_t)(*(void ***)p)[SSLOT_GetDesc])(p, d));
}

// Move one OWNING engine slot from its current surface to ours: Release the
// displaced surface exactly as the engine would have, AddRef ours for the
// Release the engine will issue. Dead (retired) pointers are just overwritten.
bool transfer(void **holder, void *ours)
{
    void *old = *holder;
    if (old == ours) {
        return false;
    }
    if (old && old != g_retired_front && old != g_retired_depth) {
        release(old);
    }
    add_ref(ours);
    *holder = ours;
    return true;
}

// The engine's render size (RenderShell +0x2b98/+0x2b9c) — every engine chain
// surface is sized from it; the carrier front/depth follow it too. Stock it
// equals the swapchain size; with render_size=WxH it is the HMD size.
bool render_dims(uint32_t *w, uint32_t *h)
{
    *w = *(volatile const uint32_t *)(MC2_G_RENDERSHELL + MC2_RS_SCREEN_W_OFF);
    *h = *(volatile const uint32_t *)(MC2_G_RENDERSHELL + MC2_RS_SCREEN_H_OFF);
    return *w != 0 && *h != 0;
}

bool create_surface(void *device, size_t slot_index, D3DSURFACE_DESC d, void **out,
                    const char *what)
{
    uint32_t rw = 0, rh = 0;
    if (render_dims(&rw, &rh)) {
        d.Width = rw; // the swapchain's desc supplies format; size = render size
        d.Height = rh;
    }
    const D3DMULTISAMPLE_TYPE ms = g_have_msaa ? g_msaa : d.MultiSampleType;
    const DWORD msq = g_have_msaa ? g_msaa_q : d.MultiSampleQuality;
    void *s = nullptr;
    const HRESULT hr = ((CreateSurf_t)(*(void ***)device)[slot_index])(
        device, d.Width, d.Height, d.Format, ms, msq, FALSE, &s, nullptr);
    if (FAILED(hr) || !s) {
        g_failures++;
        MC2VR_LOG("outputs: create %s %ux%u fmt=%u ms=%u FAILED hr=%08lx", what, d.Width,
                  d.Height, (unsigned)d.Format, (unsigned)ms, (unsigned long)hr);
        return false;
    }
    *out = s;
    MC2VR_LOG("outputs: created %s %p %ux%u fmt=%u ms=%u/%lu", what, s, d.Width, d.Height,
              (unsigned)d.Format, (unsigned)ms, (unsigned long)msq);
    return true;
}

bool table_owned()
{
    void *ref = *slot(MC2_RS_RT_FRONTREF_OFF);
    void *adr = *auto_depth_ref();
    return g_front && g_depth && *slot(MC2_RS_RT_FRONT_OFF) == g_front &&
           (ref == g_front || ref == nullptr) && *slot(MC2_RS_RT_AUTODEPTH_OFF) == g_depth &&
           (adr == g_depth || adr == nullptr);
}

// Install the carrier front/depth into every engine holder (idempotent).
void take_over(const char *reason)
{
    void **front_slot = slot(MC2_RS_RT_FRONT_OFF);
    void **ref_slot = slot(MC2_RS_RT_FRONTREF_OFF);
    void **auto_slot = slot(MC2_RS_RT_AUTODEPTH_OFF);
    void **scene_slot = slot(MC2_RS_RT_SCENEDEPTH_OFF);
    if (!*front_slot || !*auto_slot) {
        return; // table not (or no longer) set up — nothing to own
    }
    if (table_owned()) {
        return;
    }
    void *device = engine_device();
    if (!device) {
        return;
    }

    void *old_front = *front_slot;
    void *old_auto = *auto_slot;

    // Our surfaces copy the engine's own descs (behavior-neutral: same size,
    // format; MSAA = what the engine requested for its swapchain).
    if (!g_front && old_front != g_retired_front) {
        D3DSURFACE_DESC d = {};
        if (!get_desc(old_front, &d) ||
            !create_surface(device, DSLOT_CreateRenderTarget, d, &g_front, "front")) {
            return;
        }
    }
    if (!g_depth && old_auto != g_retired_depth) {
        D3DSURFACE_DESC d = {};
        if (!get_desc(old_auto, &d) ||
            !create_surface(device, DSLOT_CreateDepthStencilSurface, d, &g_depth, "depth")) {
            return;
        }
    }
    if (!g_front || !g_depth) {
        return;
    }

    // Owning holders. +0x3ea0 owns a ref only when it is NOT the auto-depth
    // alias (LtiRenderer_ReleaseRenderTargets skips it when equal).
    const bool scene_alias = (*scene_slot == old_auto && old_auto != g_depth);
    bool changed = transfer(front_slot, g_front);
    if (*ref_slot) {
        changed |= transfer(ref_slot, g_front);
    }
    changed |= transfer(auto_slot, g_depth);
    if (scene_alias) {
        *scene_slot = g_depth;
    }
    if (*auto_depth_ref()) {
        changed |= transfer(auto_depth_ref(), g_depth);
    }
    if (!changed) {
        return;
    }
    // g_DepthStencilCache / g_CurRenderTarget are left alone on purpose: they
    // mirror what is BOUND on the device, so the engine's next bind of our
    // surfaces simply misses the cache and binds.

    g_takeovers++;
    // Consistency check: the engine's main RT must be at the render size (a
    // creation path that sizes it from the swapchain would render the world
    // into a mismatched target — black world, live 2026-11-08).
    {
        D3DSURFACE_DESC md = {};
        uint32_t rw = 0, rh = 0;
        if (render_dims(&rw, &rh) && get_desc(*slot(MC2_RS_RT_TABLE_OFF), &md) &&
            (md.Width != rw || md.Height != rh)) {
            MC2VR_LOG("outputs: WARNING engine main RT is %ux%u but the render size is %ux%u "
                      "(%s) — a chain-creation path is not covered",
                      md.Width, md.Height, rw, rh, reason);
        }
    }
    MC2VR_LOG("outputs: took over engine outputs (%s, frame %llu): front %p -> %p, "
              "auto depth %p -> %p%s",
              reason, (unsigned long long)hooks::frame_count(), old_front, g_front, old_auto,
              g_depth, scene_alias ? " (+ scene-depth alias)" : "");
}

void *real_backbuffer(void *device)
{
    if (g_real_bb || !device) {
        return g_real_bb;
    }
    void *bb = nullptr;
    if (SUCCEEDED(((GetBackBuffer_t)(*(void ***)device)[DSLOT_GetBackBuffer])(
            device, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) &&
        bb) {
        release(bb); // identity only; the swapchain keeps it alive
        g_real_bb = bb;
    }
    return g_real_bb;
}

bool create_resolved(void *device, void **out, const char *what)
{
    if (*out) {
        return true;
    }
    D3DSURFACE_DESC d = {};
    if (!get_desc(g_front, &d)) {
        return false;
    }
    void *s = nullptr;
    const HRESULT hr = ((CreateSurf_t)(*(void ***)device)[DSLOT_CreateRenderTarget])(
        device, d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &s, nullptr);
    if (FAILED(hr) || !s) {
        g_failures++;
        MC2VR_LOG("outputs: create %s FAILED hr=%08lx", what, (unsigned long)hr);
        return false;
    }
    *out = s;
    MC2VR_LOG("outputs: created %s %p %ux%u fmt=%u", what, s, d.Width, d.Height,
              (unsigned)d.Format);
    return true;
}

// Aspect-fit `src` into `dst` (centered); returns false when it fills dst.
bool fit_rect(const D3DSURFACE_DESC &src, const D3DSURFACE_DESC &dst, RECT *r)
{
    const double sa = (double)src.Width / (double)src.Height;
    const double da = (double)dst.Width / (double)dst.Height;
    LONG w = (LONG)dst.Width, h = (LONG)dst.Height;
    if (sa > da) {
        h = (LONG)((double)dst.Width / sa + 0.5);
    } else if (sa < da) {
        w = (LONG)((double)dst.Height * sa + 0.5);
    }
    r->left = ((LONG)dst.Width - w) / 2;
    r->top = ((LONG)dst.Height - h) / 2;
    r->right = r->left + w;
    r->bottom = r->top + h;
    return w != (LONG)dst.Width || h != (LONG)dst.Height;
}

// ---- SetupRenderTargets wrap (thiscall, 1 stack arg; caller 0x004c0050:
// `push flag; call [vtbl+0x18]; ret`). fastcall = ECX + EDX + stack arg with
// callee cleanup — EDX is passed through untouched.

void __fastcall setup_hook(void *self, void *edx, uint32_t flag)
{
    rt_probe::on_setup_enter(self);
    g_setup_hook.fastcall<void>(self, edx, flag);
    render_size::after_setup(); // the mutated stretch re-snapped aspect
    take_over("SetupRenderTargets");
}

// RenderSystem_CheckDevice, right after the engine's device-restore calls:
// the table now names the NEW swapchain surfaces — retake before the next
// frame renders (otherwise the next Present's invariant check would, one
// frame late).
void restore_midhook(safetyhook::Context &)
{
    take_over("device restore");
}

} // namespace

void install()
{
    auto hook = SafetyHookInline::create(reinterpret_cast<uint8_t *>(MC2_SETUPRENDERTARGETS),
                                         reinterpret_cast<uint8_t *>(&setup_hook));
    if (!hook) {
        MC2VR_LOG("outputs: FATAL — SetupRenderTargets wrap install failed @ %p (error %u) "
                  "— takeover falls back to the per-Present check",
                  (void *)MC2_SETUPRENDERTARGETS, (unsigned)hook.error().type);
    } else {
        g_setup_hook = std::move(*hook);
        MC2VR_LOG("outputs: wrapped SetupRenderTargets @ %p", (void *)MC2_SETUPRENDERTARGETS);
    }

    auto mid = SafetyHookMid::create(reinterpret_cast<uint8_t *>(MC2_CHECKDEVICE_RESTORED),
                                     restore_midhook);
    if (!mid) {
        MC2VR_LOG("outputs: device-restore MidHook install failed @ %p (error %u) — "
                  "retake falls back to the next Present",
                  (void *)MC2_CHECKDEVICE_RESTORED, (unsigned)mid.error().type);
    } else {
        g_restore_mid = std::move(*mid);
        MC2VR_LOG("outputs: device-restore MidHook @ %p", (void *)MC2_CHECKDEVICE_RESTORED);
    }
}

bool active()
{
    return table_owned();
}

void *front()
{
    return active() ? g_front : nullptr;
}

void *capture_source()
{
    return g_transition_frame ? nullptr : front();
}

void *device()
{
    return engine_device();
}

void *mirror()
{
    return active() && g_mirror_valid ? g_mirror : nullptr;
}

void *resolve_front()
{
    void *dev = engine_device();
    if (!active() || !dev || !create_resolved(dev, &g_scratch, "resolve scratch")) {
        return nullptr;
    }
    return device::blit_surfaces(g_front, g_scratch) ? g_scratch : nullptr;
}

void note_requested_msaa(uint32_t type, uint32_t quality)
{
    g_have_msaa = true;
    g_msaa = (D3DMULTISAMPLE_TYPE)type;
    g_msaa_q = quality;
}

void on_present(void *device)
{
    take_over("present");
    if (!active()) {
        return;
    }
    void *bb = real_backbuffer(device);
    if (!bb) {
        return;
    }

    if (g_transition_frame) {
        // Reset transition frame: black (SwapEffect DISCARD — the backbuffer
        // must still be written before this Present).
        ((ColorFill_t)(*(void ***)device)[DSLOT_ColorFill])(device, bb, nullptr,
                                                           D3DCOLOR_XRGB(0, 0, 0));
        return;
    }

    // Source: the LEFT mirror while the stereo passes run (refreshed at 1->2);
    // otherwise (no passes) the front itself holds the latest final frame.
    const uint64_t frame = hooks::frame_count();
    void *src = (g_mirror_valid && frame - g_mirror_frame <= 2) ? g_mirror : g_front;

    D3DSURFACE_DESC sd = {}, bd = {};
    if (!get_desc(src, &sd) || !get_desc(bb, &bd)) {
        return;
    }
    RECT r;
    const bool bars = fit_rect(sd, bd, &r);
    if (sd.MultiSampleType != D3DMULTISAMPLE_NONE && bars) {
        // An MSAA resolve cannot stretch: resolve 1:1 first, then scale.
        src = resolve_front();
        if (!src) {
            return;
        }
    }
    if (bars) {
        ((ColorFill_t)(*(void ***)device)[DSLOT_ColorFill])(device, bb, nullptr,
                                                           D3DCOLOR_XRGB(0, 0, 0));
    }
    if (device::stretch_surfaces(src, nullptr, bb, &r,
                                 bars ? D3DTEXF_LINEAR : D3DTEXF_NONE)) {
        g_monitor_blits++;
    } else {
        g_failures++;
    }
}

void on_pass_boundary(uint32_t next_pass)
{
    if (next_pass == 0 && g_transition_frame) {
        g_transition_frame = false; // pass 2 of the transition frame is done
        g_transition_frames++;
        return;
    }
    if (next_pass != 2 || !active() || g_transition_frame) {
        return;
    }
    void *device = engine_device();
    if (!device || !create_resolved(device, &g_mirror, "mirror")) {
        return;
    }
    if (device::blit_surfaces(g_front, g_mirror)) {
        g_mirror_updates++;
        g_mirror_frame = hooks::frame_count();
        g_mirror_valid = true;
    } else {
        g_failures++;
    }
}

void on_reset(bool before, long hr)
{
    if (before) {
        // Engine slots were released by the engine's pre-Reset path
        // (FUN_0074d1b0 -> LtiRenderer_ReleaseRenderTargets); drop ours so no
        // D3DPOOL_DEFAULT surface survives into Reset.
        if (g_front) {
            g_retired_front = g_front;
            release(g_front);
            g_front = nullptr;
        }
        if (g_depth) {
            g_retired_depth = g_depth;
            release(g_depth);
            g_depth = nullptr;
        }
        if (g_mirror) {
            release(g_mirror);
            g_mirror = nullptr;
        }
        if (g_scratch) {
            release(g_scratch);
            g_scratch = nullptr;
        }
        g_mirror_valid = false;
        g_real_bb = nullptr;
        MC2VR_LOG("outputs: Reset — carrier surfaces released (engine still holding them: "
                  "front=%d depth=%d)",
                  *slot(MC2_RS_RT_FRONT_OFF) == g_retired_front,
                  *slot(MC2_RS_RT_AUTODEPTH_OFF) == g_retired_depth);
    } else {
        // The engine released its slot refs before Reset (else Reset fails);
        // once no slot still names the retired pointers, forget them — the
        // allocator may hand the same addresses to new surfaces.
        bool held = false;
        for (uintptr_t off : {MC2_RS_RT_FRONT_OFF, MC2_RS_RT_FRONTREF_OFF,
                              MC2_RS_RT_AUTODEPTH_OFF, MC2_RS_RT_SCENEDEPTH_OFF}) {
            void *p = *slot(off);
            held |= (p && (p == g_retired_front || p == g_retired_depth));
        }
        if (*auto_depth_ref() &&
            (*auto_depth_ref() == g_retired_front || *auto_depth_ref() == g_retired_depth)) {
            held = true;
        }
        if (!held) {
            g_retired_front = g_retired_depth = nullptr;
        }
        g_transition_frame = SUCCEEDED(hr); // next frame: not shown (see above)
        MC2VR_LOG("outputs: Reset END hr=%08lx%s — retake at the next setup/Present",
                  (unsigned long)hr, held ? " (engine slots still name released surfaces!)" : "");
    }
}

void begin_present_skip()
{
    // Only with the mirror path live: otherwise pass 2's Present would have
    // nothing fresh to show and pass 1's is the only correct one.
    if (!active()) {
        return;
    }
    volatile uint8_t *flag = (volatile uint8_t *)MC2_G_SUPPRESS_PRESENT;
    if (*flag == 0) { // engine-set (precache) suppression stays the engine's
        *flag = 1;
        g_skip_armed = true;
        g_present_skips++;
    }
}

void end_present_skip()
{
    if (g_skip_armed) {
        *(volatile uint8_t *)MC2_G_SUPPRESS_PRESENT = 0;
        g_skip_armed = false;
    }
}

void report_window()
{
    MC2VR_LOG("outputs: window active=%d takeovers=%llu monitorBlits=%llu mirrorUpdates=%llu "
              "presentSkips=%llu transitionFrames=%llu failures=%llu",
              (int)active(), (unsigned long long)g_takeovers,
              (unsigned long long)g_monitor_blits, (unsigned long long)g_mirror_updates,
              (unsigned long long)g_present_skips, (unsigned long long)g_transition_frames,
              (unsigned long long)g_failures);
    g_monitor_blits = g_mirror_updates = g_present_skips = 0;
}

} // namespace mc2vr::render_outputs
