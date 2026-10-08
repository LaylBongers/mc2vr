// HMD cull frustum — see cull_frustum.hpp for the design and
// docs/frustum_cull_plan.md for the evidence.

#include "cull_frustum.hpp"

#include <safetyhook.hpp>

#include <windows.h>

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
SafetyHookMid g_mid;

// Per-window counters (builder calls can come from any thread; the counts are
// diagnostics, a lost increment is harmless).
uint64_t g_builds = 0, g_widened = 0, g_other_view = 0, g_no_cam = 0, g_no_fov = 0;
float g_game_th = 0.0f, g_game_tv = 0.0f;  // last HMD-view game extents seen
float g_out_th = 0.0f, g_out_tv = 0.0f;    // what we wrote

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
    g_widened++;
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
}

void report_window()
{
    if (!g_enabled) {
        return;
    }
    // Oracles: widened > 0 every window while the HMD is tracked; game vs
    // out angles show the extension (game ~36.7°/22.7° stock widescreen).
    // widened == 0 with otherView > 0 = the union-camera match failed.
    MC2VR_LOG("cullfov: window: builds=%llu widened=%llu otherView=%llu noCam=%llu "
              "noFov=%llu | game %.1f°/%.1f° -> cull %.1f°/%.1f° (halfH/halfV)",
              (unsigned long long)g_builds, (unsigned long long)g_widened,
              (unsigned long long)g_other_view, (unsigned long long)g_no_cam,
              (unsigned long long)g_no_fov, deg(g_game_th), deg(g_game_tv),
              deg(g_out_th), deg(g_out_tv));
    g_builds = g_widened = g_other_view = g_no_cam = g_no_fov = 0;
}

} // namespace mc2vr::cullfov
