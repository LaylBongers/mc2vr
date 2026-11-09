// The HUD layer — see hud.hpp.

#include "vr/hud.hpp"

#include <windows.h>
#include <d3d9.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "game/dx9.hpp"
#include "game/hud.hpp"
#include "game/render_cmd.hpp"
#include "game/pg_primitive.hpp"
#include "game/render_shell.hpp"
#include "log.hpp"
#include "vr/render_outputs.hpp"
#include "vr/render_size.hpp"
#include "vr/view_rewrite.hpp"

namespace mc2vr::hud {

namespace {

float g_distance = 1.5f;  // metres
float g_size_deg = 55.0f; // horizontal angular size, seen from between the eyes

// ---- record ranges ----
struct Range {
    uint32_t lo, hi;  // record indices [lo, hi) of one build
};
constexpr int kMaxRanges = 8;
struct Ranges {
    Range markers[kMaxRanges];
    int n_markers;
    Range hud[kMaxRanges];
    int n_hud;
    bool layout_16x9;  // the HUD steps ran with the layer's screen
};

// ---- layout mode ----
// The layer target exists (render thread writes, update thread reads).
std::atomic<bool> g_target_ready{false};
// Update thread writes, update + build threads read.
std::atomic<bool> g_layout{false};
uint64_t g_relayouts = 0;

// The GUI manager's screen info ([g_GuiMgr+0x60]); +0 caches the view
// count, a mismatch re-lays out every widget (GuiMgr_CheckViewCount 0x00609e40).
constexpr uintptr_t kGuiScreenInfo = 0x60;

// RenderShell aspect mode (+0x2bc4: 0 = 4:3 STANDARD, 1 = 16:9 WIDESCREEN,
// 2/3 = script-set letterbox modes). OnScreenResize snaps it from the render
// size (4:3 for the HMD's 0.9). Readers: the GUI's aspect check (0x00617f78,
// re-lays out on a change), Lua GetScreenRatio (0x005b02c5), BeginDisplay's
// mode-3 pillarbox (0x004af05a), split-screen viewports (0x0085aa1e; single
// view ignores it). Camera aspect uses +0x2bc8/+0x2bcc, not this.
constexpr uintptr_t kAspectMode = 0x2bc4;
constexpr uintptr_t kRenderW = 0x2b98, kRenderH = 0x2b9c;
std::atomic<bool> g_mode_forced{false};

// View blocks (docs/reverse_engineering/view_blocks.md): block b at
// shell + b*0xe80; +0x2c H (int), +0x34 the UI scale H/480 (float, read by
// GUI code only). +0x2b90 = the number of blocks RenderShell_SetupViewports
// wrote (main viewport + one per player view).
constexpr uintptr_t kViewH = 0x2c, kViewUiScale = 0x34, kViewCount = 0x2b90;
constexpr uint32_t kMaxBlocks = 3;
// The stock UI scale per block, saved where the engine writes it.
std::atomic<uint32_t> g_eye_scale_bits[kMaxBlocks] = {};

// Build thread.
Ranges g_building = {};
uint32_t g_open = 0;

// Published with the record list; read by the render thread at frame start.
SRWLOCK g_lock = SRWLOCK_INIT;
Ranges g_published = {};
const void *g_published_base = nullptr;

// Render thread, this frame.
Ranges g_frame = {};
bool g_active = false;
bool g_drawn = false;  // the HUD target holds this frame's HUD
// The last frame's eye poses: the panel is head-locked, so a tracking drop
// (no pose this frame) keeps compositing with the last FOVs / IPD.
vp_camera::EyePose g_last_eyes[2] = {};
bool g_have_last_eyes = false;
// The engine's viewport and screen constants around the HUD draw.
D3DVIEWPORT9 g_saved_vp = {};
float g_saved_consts_vs[4] = {}, g_saved_consts_ps[4] = {};

// ---- the HUD target ----
IDirect3DTexture9 *g_texture = nullptr;
IDirect3DSurface9 *g_color = nullptr;  // level 0 of g_texture
IDirect3DSurface9 *g_depth = nullptr;  // D24S8: the minimap's stencil mask

// Window counters.
uint64_t g_frames = 0, g_frames_layered = 0, g_composites = 0, g_failures = 0;

void close_range(Range *ranges, int *n)
{
    const uint32_t hi = game::g_PrimWriteCount::get();
    if (hi > g_open && *n < kMaxRanges) {
        ranges[(*n)++] = {g_open, hi};
    }
}

void release_target()
{
    if (g_color != nullptr) g_color->Release();
    if (g_texture != nullptr) g_texture->Release();
    if (g_depth != nullptr) g_depth->Release();
    g_color = g_depth = nullptr;
    g_texture = nullptr;
    g_target_ready.store(false, std::memory_order_release);
    g_active = g_drawn = false;
}

bool parse_float(const char *value, float lo, float hi, float *out)
{
    char *end = nullptr;
    const float v = std::strtof(value, &end);
    if (end == value || *end != '\0' || !(v >= lo && v <= hi)) {
        return false;
    }
    *out = v;
    return true;
}

struct QuadVertex {
    float x, y, z, rhw, u, v;
};

} // namespace

bool set_distance(const char *value)
{
    if (!parse_float(value, 0.3f, 20.0f, &g_distance)) return false;
    MC2VR_LOG("hud: hud_distance=%.2f m", g_distance);
    return true;
}

bool set_size(const char *value)
{
    if (!parse_float(value, 10.0f, 120.0f, &g_size_deg)) return false;
    MC2VR_LOG("hud: hud_size=%.1f deg", g_size_deg);
    return true;
}

void begin_markers()
{
    g_open = game::g_PrimWriteCount::get();
}

void end_markers()
{
    close_range(g_building.markers, &g_building.n_markers);
}

bool layout_16x9()
{
    return g_layout.load(std::memory_order_acquire);
}

namespace {

uint32_t block_count(const uint8_t *shell)
{
    uint16_t n;
    std::memcpy(&n, shell + kViewCount, 2);
    return n < kMaxBlocks ? n : kMaxBlocks;
}

// The UI scale of every block: the layer's, or the stock H/480 back.
void apply_ui_scale(uint8_t *shell, bool layout)
{
    for (uint32_t b = 0, n = block_count(shell); b < n; b++) {
        uint8_t *const block = shell + b * game::kRenderShellViewStride;
        uint32_t bits = layout ? kUiScaleBits : g_eye_scale_bits[b].load(std::memory_order_relaxed);
        if (!layout && bits == 0) {
            int32_t h;
            std::memcpy(&h, block + kViewH, 4);
            const float scale = (float)h / 480.0f;
            std::memcpy(&bits, &scale, 4);
        }
        std::memcpy(block + kViewUiScale, &bits, 4);
    }
}

// The aspect mode the GUI and scripts see: WIDESCREEN for the layer;
// afterwards the engine's own 4:3 / 16:9 snap from the render size again.
// Script-set letterbox modes (2/3) are left alone.
void apply_aspect_mode(uint8_t *shell, bool layout)
{
    uint32_t *const mode = reinterpret_cast<uint32_t *>(shell + kAspectMode);
    if (*mode > 1) return;
    if (layout) {
        *mode = 1;
        g_mode_forced.store(true, std::memory_order_relaxed);
    } else if (g_mode_forced.exchange(false, std::memory_order_relaxed)) {
        const uint32_t w = *reinterpret_cast<const uint32_t *>(shell + kRenderW);
        const uint32_t h = *reinterpret_cast<const uint32_t *>(shell + kRenderH);
        if (w != 0 && h != 0) {
            const float a = (float)w / (float)h;
            *mode = std::fabs(a - 16.0f / 9.0f) < std::fabs(a - 4.0f / 3.0f) ? 1u : 0u;
        }
    }
}

} // namespace

void on_viewports_set(uint8_t *shell)
{
    // The engine just wrote the stock blocks: their scales are the eye image's.
    for (uint32_t b = 0, n = block_count(shell); b < n; b++) {
        uint32_t bits;
        std::memcpy(&bits, shell + b * game::kRenderShellViewStride + kViewUiScale, 4);
        g_eye_scale_bits[b].store(bits, std::memory_order_relaxed);
    }
    if (layout_16x9()) {
        apply_ui_scale(shell, true);
        apply_aspect_mode(shell, true);
    }
}

void on_screen_setup()
{
    uint8_t *const shell = game::g_RenderShellPtrB::get();
    if (shell != nullptr && layout_16x9()) apply_aspect_mode(shell, true);
}

uint32_t eye_scale_bits(uint32_t index)
{
    return index < kMaxBlocks ? g_eye_scale_bits[index].load(std::memory_order_relaxed) : 0;
}

void on_update_tick()
{
    // The mode follows the HMD's session state, which the runtime already
    // debounces (docs/render_resolution.md): no debounce here.
    const bool want = render_size::hmd_in_use() && g_target_ready.load(std::memory_order_acquire);
    if (want == g_layout.load(std::memory_order_relaxed)) {
        return;
    }
    uint8_t *const mgr = game::g_GuiMgr::get();
    uint8_t *const shell = game::g_RenderShellPtrB::get();
    uint32_t *const info = mgr != nullptr ? *reinterpret_cast<uint32_t **>(mgr + kGuiScreenInfo) : nullptr;
    if (info == nullptr || shell == nullptr) {
        return;  // no GUI manager yet; retried next tick
    }
    // Ordered so that a build flagged with the layer's layout never sees the
    // stock scale: on — policy first, then the flag; off — the reverse.
    if (want) {
        apply_ui_scale(shell, true);
        apply_aspect_mode(shell, true);
        g_layout.store(true, std::memory_order_release);
    } else {
        g_layout.store(false, std::memory_order_release);
        apply_ui_scale(shell, false);
        apply_aspect_mode(shell, false);
    }
    // The GUI screen info and the scripts' resize event follow the layout
    // (hooks/hud.cpp rewrites the size it reads)...
    game::GuiMgr_OnScreenResize(mgr);
    // ...and a bogus view count: full re-layout on the manager's next update.
    *info = 0xffffffffu;
    g_relayouts++;
    MC2VR_LOG("hud: layout -> %s (GUI re-layout)", want ? "16:9 HUD layer" : "eye image (stock)");
}

void begin_hud()
{
    g_open = game::g_PrimWriteCount::get();
    g_building.layout_16x9 = layout_16x9();
}

void end_hud()
{
    close_range(g_building.hud, &g_building.n_hud);
}

void on_publish()
{
    AcquireSRWLockExclusive(&g_lock);
    g_published = g_building;
    g_published_base = game::g_PrimitiveBase::get();
    ReleaseSRWLockExclusive(&g_lock);
    g_building = {};
}

void begin_frame()
{
    AcquireSRWLockShared(&g_lock);
    g_frame = g_published;
    const void *const base = g_published_base;
    ReleaseSRWLockShared(&g_lock);
    g_drawn = false;
    vp_camera::EyePose eyes[2];
    if (view_rewrite::frame_eyes(eyes)) {
        g_last_eyes[0] = eyes[0];
        g_last_eyes[1] = eyes[1];
        g_have_last_eyes = true;
    }
    // A build laid out for the layer is drawn as the layer (at a mode switch
    // the one or two frames in between keep the stock path).
    g_active = g_texture != nullptr && g_have_last_eyes && base == game::g_PrimitiveBase::get() &&
               g_frame.n_hud > 0 && g_frame.layout_16x9;
    g_frames++;
    if (g_active) g_frames_layered++;
}

bool active()
{
    return g_active;
}

bool is_hud_record(uint32_t index)
{
    for (int i = 0; i < g_frame.n_hud; i++) {
        if (index >= g_frame.hud[i].lo && index < g_frame.hud[i].hi) return true;
    }
    return false;
}

void begin_draw()
{
    IDirect3DDevice9 *const dev = render_outputs::device();
    dev->GetViewport(&g_saved_vp);  // before the bind: SetRenderTarget resets it
    game::Dx9_SetRenderTarget(0, g_color);
    game::Dx9_SetDepthStencilSurface(g_depth, 0);
    // The layer's own screen: viewport and pixel-to-clip constants for
    // kLayoutW x kLayoutH, as cmd op 8 would set them. The engine-2D HUD
    // records carry no op 8 of their own (they inherit the view context's).
    // The constants reach the shaders on the next technique/pass change, so
    // the last-technique/pass caches are dropped.
    std::memcpy(g_saved_consts_vs, game::g_ScreenConstsVS::ptr(), sizeof g_saved_consts_vs);
    std::memcpy(g_saved_consts_ps, game::g_ScreenConstsPS::ptr(), sizeof g_saved_consts_ps);
    game::RenderCmd_SetScreenConstants(0, 0, kLayoutW, kLayoutH, g_saved_vp.MinZ, g_saved_vp.MaxZ, nullptr,
                                       nullptr);
    game::g_LastTechnique::set(nullptr);
    game::g_LastPass::set(nullptr);
    dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL, 0x00000000, 1.0f, 0);
    // Coverage into a transparent target: alpha accumulates as
    // a + dst*(1-a), so the result composites premultiplied. The engine never
    // sets these states.
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
    dev->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
}

void end_draw()
{
    IDirect3DDevice9 *const dev = render_outputs::device();
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
    dev->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_ZERO);
    game::Dx9_SetRenderTarget(0, render_outputs::front());
    game::Dx9_SetDepthStencilSurface(render_outputs::depth(), 0);
    game::RenderCmd_SetScreenConstants(g_saved_vp.X, g_saved_vp.Y, g_saved_vp.Width, g_saved_vp.Height,
                                       g_saved_vp.MinZ, g_saved_vp.MaxZ, nullptr, nullptr);
    std::memcpy(const_cast<uint8_t *>(game::g_ScreenConstsVS::ptr()), g_saved_consts_vs, sizeof g_saved_consts_vs);
    std::memcpy(const_cast<uint8_t *>(game::g_ScreenConstsPS::ptr()), g_saved_consts_ps, sizeof g_saved_consts_ps);
    game::g_LastTechnique::set(nullptr);
    game::g_LastPass::set(nullptr);
    g_drawn = true;
}

void composite(uint32_t pass)
{
    if (!g_active || !g_drawn || (pass != 1 && pass != 2)) {
        return;
    }
    const vp_camera::EyePose *const eyes = g_last_eyes;  // this frame's, or the last ones
    IDirect3DDevice9 *const dev = render_outputs::device();
    IDirect3DSurface9 *const front = render_outputs::front();
    D3DSURFACE_DESC fd = {}, hd = {};
    if (dev == nullptr || front == nullptr || FAILED(front->GetDesc(&fd)) ||
        FAILED(g_color->GetDesc(&hd))) {
        g_failures++;
        return;
    }

    // A plane hud_distance ahead of the head, centered, hud_size wide, with
    // the HUD target's aspect; seen by this eye at x = -+ipd/2. Eye canting
    // is ignored (parallel displays).
    const vp_camera::EyePose &eye = eyes[pass - 1];
    const float dx = eyes[1].pos.x - eyes[0].pos.x, dy = eyes[1].pos.y - eyes[0].pos.y,
                dz = eyes[1].pos.z - eyes[0].pos.z;
    const float ipd = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float ex = pass == 1 ? -0.5f * ipd : 0.5f * ipd;
    const float D = g_distance;
    const float hw = D * std::tan(0.5f * g_size_deg * 3.14159265f / 180.0f);
    const float hh = hw * (float)hd.Height / (float)hd.Width;
    const float tl = std::tan(eye.fov_left), tr = std::tan(eye.fov_right);
    const float tu = std::tan(eye.fov_up), td = std::tan(eye.fov_down);
    const float W = (float)fd.Width, H = (float)fd.Height;
    const float x0 = (((-hw - ex) / D) - tl) / (tr - tl) * W - 0.5f;
    const float x1 = (((hw - ex) / D) - tl) / (tr - tl) * W - 0.5f;
    const float y0 = (tu - hh / D) / (tu - td) * H - 0.5f;
    const float y1 = (tu + hh / D) / (tu - td) * H - 0.5f;
    const QuadVertex quad[4] = {
        {x0, y0, 0.0f, 1.0f, 0.0f, 0.0f},
        {x1, y0, 0.0f, 1.0f, 1.0f, 0.0f},
        {x0, y1, 0.0f, 1.0f, 0.0f, 1.0f},
        {x1, y1, 0.0f, 1.0f, 1.0f, 1.0f},
    };

    // Everything is restored afterwards: the state block covers the device
    // state, the targets go back to what the engine's bind caches say (through
    // the device hooks, so scene MSAA maps them again).
    IDirect3DStateBlock9 *state = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &state)) || state == nullptr) {
        g_failures++;
        return;
    }
    IDirect3DSurface9 *const engine_rt = game::g_CurRenderTarget::at(0);
    IDirect3DSurface9 *const engine_ds = game::g_DepthStencilCache::get();

    dev->SetRenderTarget(0, front);
    dev->SetDepthStencilSurface(nullptr);
    dev->BeginScene();
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    dev->SetTexture(0, g_texture);
    dev->SetTexture(1, nullptr);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);  // premultiplied
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    const HRESULT hr = dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(QuadVertex));
    dev->EndScene();

    if (engine_rt != nullptr) dev->SetRenderTarget(0, engine_rt);
    dev->SetDepthStencilSurface(engine_ds);
    state->Apply();
    state->Release();
    if (SUCCEEDED(hr)) g_composites++;
    else g_failures++;
}

void ensure(IDirect3DDevice9 *device)
{
    if (device == nullptr || g_color != nullptr) {
        return;
    }
    const UINT w = kLayoutW, h = kLayoutH;
    HRESULT hr = device->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                       D3DPOOL_DEFAULT, &g_texture, nullptr);
    if (SUCCEEDED(hr)) hr = g_texture->GetSurfaceLevel(0, &g_color);
    if (SUCCEEDED(hr)) {
        hr = device->CreateDepthStencilSurface(w, h, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE,
                                               &g_depth, nullptr);
    }
    if (FAILED(hr)) {
        g_failures++;
        MC2VR_LOG("hud: create HUD target %ux%u FAILED hr=%08lx — the HUD stays in the eye images", w, h, (unsigned long)hr);
        release_target();
        return;
    }
    g_target_ready.store(true, std::memory_order_release);
    MC2VR_LOG("hud: created HUD target %ux%u (distance %.2f m, %.1f deg wide)", w, h, g_distance,
              g_size_deg);
}

void on_reset_before()
{
    release_target();
}

void report_window()
{
    MC2VR_LOG("hud: frames layered=%llu of %llu, composites=%llu, failures=%llu, layout=%s, relayouts=%llu",
              (unsigned long long)g_frames_layered, (unsigned long long)g_frames, (unsigned long long)g_composites,
              (unsigned long long)g_failures, layout_16x9() ? "16:9" : "stock", (unsigned long long)g_relayouts);
    g_frames = g_frames_layered = g_composites = g_failures = 0;
}

} // namespace mc2vr::hud
