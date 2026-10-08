// HMD cull frustum — see cull_frustum.hpp for the design and
// docs/frustum_cull_plan.md for the evidence.

#include "cull_frustum.hpp"

#include <safetyhook.hpp>

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "game_addresses.h"
#include "log.hpp"
#include "view_table.hpp"

namespace mc2vr::cullfov {

namespace {

bool g_enabled = false;
float g_margin_deg = 5.0f;
SafetyHookMid g_mid, g_clear_mid;

// Per-window counters (builder calls can come from any thread; the counts are
// diagnostics, a lost increment is harmless).
uint64_t g_builds = 0, g_widened = 0, g_other_view = 0, g_no_cam = 0, g_no_fov = 0;
float g_game_th = 0.0f, g_game_tv = 0.0f;  // last HMD-view game extents seen
float g_out_th = 0.0f, g_out_tv = 0.0f;    // what we wrote
uint64_t g_clear_restored = 0, g_clear_stock = 0;

// The last widened view's (game, written) extent magnitudes, published to the
// camera thread (camclear_midhook). Written by the builder (render thread).
std::atomic<float> g_pub_game_th{0.0f}, g_pub_game_tv{0.0f};
std::atomic<float> g_pub_out_th{0.0f}, g_pub_out_tv{0.0f};

constexpr float DEG = 0.017453293f;
constexpr float MAX_HALF = 85.0f * DEG;  // tan pole guard

void tans_midhook(safetyhook::Context &ctx)
{
    if (!g_enabled) {
        return;
    }
    g_builds++;

    // EBX = the view render-ctx; [ctx+0x28] = camera object, a self-indexed
    // 0x70-stride entry array (active entry = cam + [cam]*0x70; rotation rows
    // at +0x10, position at +0x40). NULL = the builder's identity default.
    const uintptr_t vctx = ctx.ebx;
    const uintptr_t cam = *(const uint32_t *)(vctx + MC2_VCCAM_CTX_CAMARRAY_OFF);
    if (cam == 0) {
        g_no_cam++;
        return;
    }
    const uint32_t idx = *(const uint32_t *)cam;
    if (idx >= MC2_CAMTABLE_SLOT_STRIDE / MC2_VCCAM_ENTRY_STRIDE) {
        g_no_cam++;
        return;
    }
    const uintptr_t entry = cam + idx * MC2_VCCAM_ENTRY_STRIDE;
    if (!camtable::is_union_camera((const float *)(entry + MC2_VCCAM_ENTRY_ROT_OFF),
                                   (const float *)(entry + MC2_VCCAM_ENTRY_POS_OFF))) {
        g_other_view++;
        return;
    }

    float half_h, half_v;
    if (!camtable::get_fov_union(&half_h, &half_v)) {
        g_no_fov++;
        return;
    }
    const float margin = g_margin_deg * DEG;
    const float hmd_th = std::tan(std::fmin(half_h + margin, MAX_HALF));
    const float hmd_tv = std::tan(std::fmin(half_v + margin, MAX_HALF));

    // XMM2 = tanH, XMM0 = tanV (the builder's own signs kept; magnitudes
    // widened, never narrowed).
    float &th = ctx.xmm2.f32[0];
    float &tv = ctx.xmm0.f32[0];
    g_game_th = th;
    g_game_tv = tv;
    th = std::copysign(std::fmax(std::fabs(th), hmd_th), th);
    tv = std::copysign(std::fmax(std::fabs(tv), hmd_tv), tv);
    memcpy((void *)(vctx + MC2_VCCAM_CTX_TANH_OFF), &th, 4);
    memcpy((void *)(vctx + MC2_VCCAM_CTX_TANV_OFF), &tv, 4);
    g_out_th = th;
    g_out_tv = tv;
    g_pub_game_th.store(std::fabs(g_game_th), std::memory_order_relaxed);
    g_pub_game_tv.store(std::fabs(g_game_tv), std::memory_order_relaxed);
    g_pub_out_th.store(std::fabs(th), std::memory_order_relaxed);
    g_pub_out_tv.store(std::fabs(tv), std::memory_order_relaxed);
    g_widened++;
}

// Third-person camera clearance (FUN_007107d0, camera thread): runs right after
// the camera copied the main view's frustum struct onto its stack. If the
// copy's near quad has the extents we wrote, shrink its four near corners back
// to the game's own extents — the camera's obstacle clearance stays stock
// while the cull keeps the HMD frustum. Only the private copy is touched.
void camclear_midhook(safetyhook::Context &ctx)
{
    if (!g_enabled) {
        return;
    }
    const float out_th = g_pub_out_th.load(std::memory_order_relaxed);
    const float out_tv = g_pub_out_tv.load(std::memory_order_relaxed);
    const float game_th = g_pub_game_th.load(std::memory_order_relaxed);
    const float game_tv = g_pub_game_tv.load(std::memory_order_relaxed);
    if (!(out_th > 0.0f) || !(out_tv > 0.0f)) {
        g_clear_stock++;
        return;  // nothing widened yet
    }
    float *f = (float *)(ctx.esp + MC2_CAMCLEAR_COPY_ESP_OFF);
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
    const float sh = game_th / out_th, sv = game_tv / out_tv;
    static const float SIGN_H[4] = {1, 1, -1, -1}, SIGN_V[4] = {1, -1, -1, 1};
    for (int i = 0; i < 4; i++) {
        for (int k = 0; k < 3; k++) {
            c[i * 3 + k] = center[k] + SIGN_H[i] * sh * half_h[k] + SIGN_V[i] * sv * half_v[k];
        }
    }
    g_clear_restored++;
}

bool parse_on_off(const char *value, bool *on)
{
    if (strcmp(value, "on") == 0) {
        *on = true;
    } else if (strcmp(value, "off") == 0) {
        *on = false;
    } else {
        return false;
    }
    return true;
}

double deg(float tan_value)
{
    return std::atan(std::fabs((double)tan_value)) * 57.29577951;
}

} // namespace

bool set_enabled(const char *value)
{
    bool on;
    if (!parse_on_off(value, &on)) {
        return false;
    }
    g_enabled = on;
    MC2VR_LOG("cullfov: HMD cull frustum %s", on ? "ARMED" : "disabled");
    return true;
}

bool set_margin(double degrees)
{
    if (!(degrees >= 0.0) || degrees > 30.0) {
        MC2VR_LOG("cullfov: cull_fov_margin=%.3f out of range [0,30] — kept %.1f",
                  degrees, (double)g_margin_deg);
        return false;
    }
    g_margin_deg = (float)degrees;
    MC2VR_LOG("cullfov: margin = %.1f°", degrees);
    return true;
}

void install()
{
    auto mid = SafetyHookMid::create(reinterpret_cast<uint8_t *>(MC2_VCCAM_TANS_READY),
                                     tans_midhook);
    if (!mid) {
        MC2VR_LOG("cullfov: builder MidHook install FAILED @ %p (error %u) — "
                  "culling keeps the game frustum",
                  (void *)MC2_VCCAM_TANS_READY, (unsigned)mid.error().type);
        return;
    }
    g_mid = std::move(*mid);
    MC2VR_LOG("cullfov: installed builder MidHook @ %p (%s)",
              (void *)MC2_VCCAM_TANS_READY, g_enabled ? "ARMED" : "idle");

    auto cmid = SafetyHookMid::create(reinterpret_cast<uint8_t *>(MC2_CAMCLEAR_FRUSTUM_COPIED),
                                      camclear_midhook);
    if (!cmid) {
        MC2VR_LOG("cullfov: camera-clearance MidHook install FAILED @ %p (error %u) — "
                  "the camera sees the HMD near plane (pull-in)",
                  (void *)MC2_CAMCLEAR_FRUSTUM_COPIED, (unsigned)cmid.error().type);
        return;
    }
    g_clear_mid = std::move(*cmid);
    MC2VR_LOG("cullfov: installed camera-clearance MidHook @ %p",
              (void *)MC2_CAMCLEAR_FRUSTUM_COPIED);
}

void report_window()
{
    if (!g_enabled) {
        return;
    }
    // Oracles: widened > 0 every window while the HMD is tracked; game vs
    // out angles show the extension (game ~36.7°/22.7° stock widescreen).
    // widened == 0 with otherView > 0 = the union-camera match failed.
    // camClear restored > 0 while widened > 0 = the camera clearance is kept
    // stock; stock-only while widened > 0 = the copy match failed (pull-in).
    MC2VR_LOG("cullfov: window: builds=%llu widened=%llu otherView=%llu noCam=%llu "
              "noFov=%llu | game %.1f°/%.1f° -> cull %.1f°/%.1f° (halfH/halfV) | "
              "camClear restored=%llu stock=%llu",
              (unsigned long long)g_builds, (unsigned long long)g_widened,
              (unsigned long long)g_other_view, (unsigned long long)g_no_cam,
              (unsigned long long)g_no_fov, deg(g_game_th), deg(g_game_tv),
              deg(g_out_th), deg(g_out_tv), (unsigned long long)g_clear_restored,
              (unsigned long long)g_clear_stock);
    g_builds = g_widened = g_other_view = g_no_cam = g_no_fov = 0;
    g_clear_restored = g_clear_stock = 0;
}

} // namespace mc2vr::cullfov
