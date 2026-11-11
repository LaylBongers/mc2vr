// HMD cull frustum — see cull_frustum.hpp for the design and
// docs/culling.md for the evidence.

#include "vr/cull_frustum.hpp"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "game/addresses.h"
#include "log.hpp"
#include "vr/camera_table.hpp"

namespace mc2vr::cull_frustum {

namespace {


// Per-window counters (builder calls can come from any thread; the counts are
// diagnostics, a lost increment is harmless).
uint64_t g_builds = 0, g_widened = 0, g_other_view = 0, g_no_cam = 0, g_no_fov = 0;
float g_game_th = 0.0f, g_game_tv = 0.0f;  // last HMD-view game extents seen
float g_out_th = 0.0f, g_out_tv = 0.0f;    // what we wrote
float g_clear_th = 0.0f, g_clear_tv = 0.0f;  // the camera clearance's reference extents
uint64_t g_clear_restored = 0, g_clear_stock = 0;

// Views whose camera isn't the union camera (they keep the game's extents),
// per view context, per window: how far their camera is from the HMD camera,
// its forward-axis angle to it, and the game's extents (docs/culling.md
// "Unmatched views").
struct OtherView {
    uintptr_t vctx;
    uint64_t builds, no_offset;
    float dist_min, dist_max, ang_min, ang_max;
    float th, tv;  // last game extents (tan)
};
constexpr int kMaxOtherViews = 8;
OtherView g_others[kMaxOtherViews];
int g_n_others = 0;
uint64_t g_others_dropped = 0;

OtherView *other_view_of(uintptr_t vctx)
{
    for (int i = 0; i < g_n_others; i++) {
        if (g_others[i].vctx == vctx) return &g_others[i];
    }
    if (g_n_others == kMaxOtherViews) {
        g_others_dropped++;
        return nullptr;
    }
    OtherView &o = g_others[g_n_others++];
    o = OtherView{};
    o.vctx = vctx;
    o.dist_min = o.ang_min = 1e30f;
    return &o;
}

// The builder's own signs kept; magnitudes widened, never narrowed. Writes the
// ctx tans too (render constants + snapshot read them).
void widen(uintptr_t vctx, float &th, float &tv, float hmd_th, float hmd_tv)
{
    th = std::copysign(std::fmax(std::fabs(th), hmd_th), th);
    tv = std::copysign(std::fmax(std::fabs(tv), hmd_tv), tv);
    memcpy((void *)(vctx + MC2_VCCAM_CTX_TANH_OFF), &th, 4);
    memcpy((void *)(vctx + MC2_VCCAM_CTX_TANV_OFF), &tv, 4);
}

// The last widened view's (clearance reference, written) extent magnitudes,
// published to the camera thread (on_camera_frustum_copied). Written by the
// builder (render thread).
std::atomic<float> g_pub_clear_th{0.0f}, g_pub_clear_tv{0.0f};
std::atomic<float> g_pub_out_th{0.0f}, g_pub_out_tv{0.0f};

constexpr float DEG = 0.017453293f;
constexpr float MAX_HALF = 85.0f * DEG;  // tan pole guard

// The aspect the camera clearance is kept at. In VR the engine builds its
// views at the eye's render aspect (0.9 on the reference HMD, render_size),
// so its own extents are a narrow 18.5°/20.4° quad, not the 16:9 one the
// third-person camera was tuned against (36.7°/22.7°): the clearance would
// shrink with the render size. The 16:9 reference keeps the camera as it is
// on a 16:9 monitor, ADS zoom included.
constexpr float CLEAR_REF_ASPECT = 16.0f / 9.0f;

// The builder computes halfH = fov * 0.375 * aspect, tanH = tan(halfH),
// tanV = tanH / aspect (docs/culling.md); so the game's own pair gives
// aspect = tanH / tanV and fov * 0.375 = atan(tanH) / aspect, and the same
// fov at the reference aspect follows. Magnitudes in and out.
bool clearance_extents(float game_th, float game_tv, float *th, float *tv)
{
    if (!(game_th > 0.0f) || !(game_tv > 0.0f)) {
        return false;
    }
    const float aspect = game_th / game_tv;
    const float half = std::atan(game_th) / aspect * CLEAR_REF_ASPECT;
    if (!(half > 0.0f) || half > MAX_HALF) {
        return false;
    }
    *th = std::tan(half);
    *tv = *th / CLEAR_REF_ASPECT;
    return true;
}

} // namespace

bool on_tans_ready(uintptr_t vctx, float &th, float &tv)
{
    g_builds++;

    // [vctx+0x28] = camera object, a self-indexed 0x70-stride entry array
    // (active entry = cam + [cam]*0x70; rotation rows at +0x10, position at
    // +0x40). NULL = the builder's identity default.
    const uintptr_t cam = *(const uint32_t *)(vctx + MC2_VCCAM_CTX_CAMARRAY_OFF);
    if (cam == 0) {
        g_no_cam++;
        return false;
    }
    const uint32_t idx = *(const uint32_t *)cam;
    if (idx >= MC2_CAMTABLE_SLOT_STRIDE / MC2_VCCAM_ENTRY_STRIDE) {
        g_no_cam++;
        return false;
    }
    const uintptr_t entry = cam + idx * MC2_VCCAM_ENTRY_STRIDE;
    const float *rows = (const float *)(entry + MC2_VCCAM_ENTRY_ROT_OFF);
    const float *pos = (const float *)(entry + MC2_VCCAM_ENTRY_POS_OFF);
    if (!camera_table::is_union_camera(rows, pos)) {
        g_other_view++;
        OtherView *o = other_view_of(vctx);
        float dist = 0.0f, ang = 0.0f;
        const bool have = camera_table::union_offset(rows, pos, &dist, &ang);
        if (o != nullptr) {
            o->builds++;
            o->th = th;
            o->tv = tv;
            if (!have) {
                o->no_offset++;
            } else {
                o->dist_min = std::fmin(o->dist_min, dist);
                o->dist_max = std::fmax(o->dist_max, dist);
                o->ang_min = std::fmin(o->ang_min, ang);
                o->ang_max = std::fmax(o->ang_max, ang);
            }
        }
        return false;
    }

    float half_h, half_v;
    if (!camera_table::get_fov_union(&half_h, &half_v)) {
        g_no_fov++;
        return false;
    }
    // Exactly the eyes' union, no margin: anything wider is off-screen in
    // both eyes, yet gets occlusion boxes there, which count 0 pixels and read
    // as occluded; such objects then stay undrawn for frames after they turn
    // into view (docs/occluder_boxes.md).
    const float hmd_th = std::tan(std::fmin(half_h, MAX_HALF));
    const float hmd_tv = std::tan(std::fmin(half_v, MAX_HALF));

    g_game_th = th;
    g_game_tv = tv;
    widen(vctx, th, tv, hmd_th, hmd_tv);
    g_out_th = th;
    g_out_tv = tv;
    float clear_th, clear_tv;
    if (!clearance_extents(std::fabs(g_game_th), std::fabs(g_game_tv), &clear_th, &clear_tv)) {
        clear_th = std::fabs(g_game_th);  // degenerate game extents: the game's own
        clear_tv = std::fabs(g_game_tv);
    }
    g_clear_th = clear_th;
    g_clear_tv = clear_tv;
    g_pub_clear_th.store(clear_th, std::memory_order_relaxed);
    g_pub_clear_tv.store(clear_tv, std::memory_order_relaxed);
    g_pub_out_th.store(std::fabs(th), std::memory_order_relaxed);
    g_pub_out_tv.store(std::fabs(tv), std::memory_order_relaxed);
    g_widened++;
    return true;
}

// Third-person camera clearance: if the copy's near quad has the extents we
// wrote, rescale its four near corners to the 16:9 reference extents of the
// game's fov (clearance_extents) — the camera's obstacle clearance stays as on
// a 16:9 monitor while the cull keeps the HMD frustum. Only the private copy
// is touched.
void on_camera_frustum_copied(float *f)
{
    const float out_th = g_pub_out_th.load(std::memory_order_relaxed);
    const float out_tv = g_pub_out_tv.load(std::memory_order_relaxed);
    const float clear_th = g_pub_clear_th.load(std::memory_order_relaxed);
    const float clear_tv = g_pub_clear_tv.load(std::memory_order_relaxed);
    if (!(out_th > 0.0f) || !(out_tv > 0.0f)) {
        g_clear_stock++;
        return;  // nothing widened yet
    }
    const float near_d = f[MC2_FRUSTUM_NEAR_OFF / 4];
    float *c = f + MC2_FRUSTUM_CORNERS_OFF / 4;  // c[0..11] = near corners 0..3
    if (!(near_d > 0.0f)) {
        g_clear_stock++;
        return;
    }
    // Corners: 0 (+H,+V), 1 (+H,-V), 2 (-H,-V), 3 (-H,+V).
    float center[3], half_h[3], half_v[3];
    for (int k = 0; k < 3; k++) {
        center[k] = 0.5f * (c[0 + k] + c[6 + k]);
        half_h[k] = 0.5f * (c[0 + k] - c[9 + k]);
        half_v[k] = 0.5f * (c[0 + k] - c[3 + k]);
    }
    const float cur_th = std::sqrt(half_h[0] * half_h[0] + half_h[1] * half_h[1] +
                                   half_h[2] * half_h[2]) / near_d;
    const float cur_tv = std::sqrt(half_v[0] * half_v[0] + half_v[1] * half_v[1] +
                                   half_v[2] * half_v[2]) / near_d;
    // Ours iff the copy carries the extents we wrote (1%: float round-trip
    // through world-space corners). Anything else is the game's own frustum.
    if (std::fabs(cur_th - out_th) > 0.01f * out_th ||
        std::fabs(cur_tv - out_tv) > 0.01f * out_tv) {
        g_clear_stock++;
        return;
    }
    const float sh = clear_th / out_th, sv = clear_tv / out_tv;
    static const float SIGN_H[4] = {1, 1, -1, -1}, SIGN_V[4] = {1, -1, -1, 1};
    for (int i = 0; i < 4; i++) {
        for (int k = 0; k < 3; k++) {
            c[i * 3 + k] = center[k] + SIGN_H[i] * sh * half_h[k] + SIGN_V[i] * sv * half_v[k];
        }
    }
    g_clear_restored++;
}

namespace {

double deg(float tan_value)
{
    return std::atan(std::fabs((double)tan_value)) * 57.29577951;
}

} // namespace

void report_window()
{
    // Oracles: widened > 0 every window while the HMD is tracked; game vs
    // out angles show the extension (game ~36.7°/22.7° stock widescreen).
    // widened == 0 with otherView > 0 = the union-camera match failed.
    // camClear restored > 0 while widened > 0 = the camera clearance is kept
    // at the 16:9 reference (camClear angles, ~36.7°/22.7° unzoomed);
    // stock-only while widened > 0 = the copy match failed (pull-in).
    MC2VR_LOG("cullfov: window: builds=%llu widened=%llu otherView=%llu noCam=%llu "
              "noFov=%llu | game %.1f°/%.1f° -> cull %.1f°/%.1f° (halfH/halfV) | "
              "camClear %.1f°/%.1f° restored=%llu stock=%llu",
              (unsigned long long)g_builds, (unsigned long long)g_widened,
              (unsigned long long)g_other_view, (unsigned long long)g_no_cam,
              (unsigned long long)g_no_fov, deg(g_game_th), deg(g_game_tv),
              deg(g_out_th), deg(g_out_tv), deg(g_clear_th), deg(g_clear_tv),
              (unsigned long long)g_clear_restored, (unsigned long long)g_clear_stock);
    for (int i = 0; i < g_n_others; i++) {
        const OtherView &o = g_others[i];
        if (o.builds == 0) continue;
        MC2VR_LOG("cullfov:   other view ctx=%p builds=%llu game %.1f°/%.1f° | from the HMD camera: dist %.2f..%.2f m, "
                  "forward %.1f..%.1f° (no offset %llu)",
                  (void *)o.vctx, (unsigned long long)o.builds, deg(o.th), deg(o.tv),
                  o.dist_min < 1e29f ? o.dist_min : -1.0f, o.dist_max, o.ang_min < 1e29f ? o.ang_min : -1.0f, o.ang_max,
                  (unsigned long long)o.no_offset);
    }
    if (g_others_dropped) {
        MC2VR_LOG("cullfov:   other views beyond %d not tracked: %llu builds", kMaxOtherViews,
                  (unsigned long long)g_others_dropped);
    }
    g_n_others = 0;
    g_others_dropped = 0;
    g_builds = g_widened = g_other_view = g_no_cam = g_no_fov = 0;
    g_clear_restored = g_clear_stock = 0;
}

} // namespace mc2vr::cull_frustum
