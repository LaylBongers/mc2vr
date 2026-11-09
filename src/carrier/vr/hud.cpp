// The HUD layer — see hud.hpp.

#include "vr/hud.hpp"

#include <windows.h>
#include <d3d9.h>

#include <cmath>
#include <cstdlib>

#include "game/dx9.hpp"
#include "game/pg_primitive.hpp"
#include "game/render_shell.hpp"
#include "log.hpp"
#include "vr/render_outputs.hpp"
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
};

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

void begin_hud()
{
    g_open = game::g_PrimWriteCount::get();
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
    g_active = g_texture != nullptr && base == game::g_PrimitiveBase::get() && g_frame.n_hud > 0 &&
               view_rewrite::current_pose_id() != 0;
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
    game::Dx9_SetRenderTarget(0, g_color);
    game::Dx9_SetDepthStencilSurface(g_depth, 0);
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
    g_drawn = true;
}

void composite(uint32_t pass)
{
    if (!g_active || !g_drawn || (pass != 1 && pass != 2)) {
        return;
    }
    vp_camera::EyePose eyes[2];
    IDirect3DDevice9 *const dev = render_outputs::device();
    IDirect3DSurface9 *const front = render_outputs::front();
    D3DSURFACE_DESC fd = {}, hd = {};
    if (!view_rewrite::frame_eyes(eyes) || dev == nullptr || front == nullptr || FAILED(front->GetDesc(&fd)) ||
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
    IDirect3DSurface9 *const front = render_outputs::front();
    D3DSURFACE_DESC fd = {};
    if (device == nullptr || front == nullptr || FAILED(front->GetDesc(&fd))) {
        return;
    }
    if (g_color != nullptr) {
        D3DSURFACE_DESC cd = {};
        if (SUCCEEDED(g_color->GetDesc(&cd)) && cd.Width == fd.Width && cd.Height == fd.Height) {
            return;
        }
        release_target();
    }
    HRESULT hr = device->CreateTexture(fd.Width, fd.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                       D3DPOOL_DEFAULT, &g_texture, nullptr);
    if (SUCCEEDED(hr)) hr = g_texture->GetSurfaceLevel(0, &g_color);
    if (SUCCEEDED(hr)) {
        hr = device->CreateDepthStencilSurface(fd.Width, fd.Height, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE,
                                               &g_depth, nullptr);
    }
    if (FAILED(hr)) {
        g_failures++;
        MC2VR_LOG("hud: create HUD target %ux%u FAILED hr=%08lx — the HUD stays in the eye images", fd.Width,
                  fd.Height, (unsigned long)hr);
        release_target();
        return;
    }
    MC2VR_LOG("hud: created HUD target %ux%u (distance %.2f m, %.1f deg wide)", fd.Width, fd.Height, g_distance,
              g_size_deg);
}

void on_reset_before()
{
    release_target();
}

void report_window()
{
    MC2VR_LOG("hud: frames layered=%llu of %llu, composites=%llu, failures=%llu",
              (unsigned long long)g_frames_layered, (unsigned long long)g_frames, (unsigned long long)g_composites,
              (unsigned long long)g_failures);
    g_frames = g_frames_layered = g_composites = g_failures = 0;
}

} // namespace mc2vr::hud
