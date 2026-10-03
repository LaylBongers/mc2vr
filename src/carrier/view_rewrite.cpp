// Per-eye camera injection — see view_rewrite.hpp and docs/stereo_design.md §S2.

#include "view_rewrite.hpp"

#include <safetyhook.hpp>

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "game_addresses.h"
#include "hooks.hpp"
#include "log.hpp"

namespace mc2vr::view {

namespace {

constexpr uint32_t VS_ROWS = 256;  // vs_3_0 float constant registers (plus headroom)
constexpr float DEFAULT_AMP = 4.0f;

enum class RewriteMode { Off, On, Pulse };
RewriteMode g_mode = RewriteMode::Off;
float g_amp = DEFAULT_AMP;

// ---- live technique constant map (published by the upload-gate MidHook) ------
// Register bases slide per technique and different techniques reuse the same
// numbers, so the exact (reg, count) pairs come from the engine's own
// resolver (Technique_ResolveConstantRegisters):
//   [tech+0xd4] viewContextData reg   [tech+0xd8] count (4, 5 or 6)
//   [tech+0xdc] ViewProj reg          [tech+0xe0] count (4)
// The MidHook fires on the same thread immediately before the wrapper ->
// device VmtHook call, so on_set_vs_constant always sees the map for the
// CURRENT upload.
constexpr uint32_t REG_INVALID = 0xffffffffu;
volatile uint32_t g_vcd_reg = REG_INVALID;
volatile uint32_t g_vcd_count = 0;
volatile uint32_t g_vp_reg = REG_INVALID;
volatile uint32_t g_vp_count = 0;
uintptr_t g_tech_seen[32];  // distinct technique objects (log once each)
uint32_t g_tech_seen_n = 0;
SafetyHookMid g_vcd_mid;

// ---- pass gate (RT0 size) -----------------------------------------------------
// Shadow-map, reflection and other offscreen passes upload their own
// viewContextData; shifting those would move e.g. the shadow map relative to
// its receivers. Gate on RT0 == backbuffer size. (If the render resolution
// ever differs from the backbuffer, key this on RT identity instead.)
uint32_t g_main_w = 0, g_main_h = 0;
uint32_t g_rt_w = 0, g_rt_h = 0;
uint32_t g_rt_seen[16][2];  // distinct RT0 sizes (log once each)
uint32_t g_rt_seen_n = 0;

bool pass_is_main()
{
    // Unknown main size (params not yet read): don't gate.
    return g_main_w == 0 || (g_rt_w == g_main_w && g_rt_h == g_main_h);
}

// ---- window counters ------------------------------------------------------------
uint64_t g_vs_calls = 0, g_vs_vec4s = 0, g_rows_rewritten = 0;

// MidHook @ MC2_VCD_UPLOAD_CMP (`cmp [edi+0xd8],0` at the upload gate):
// EDI = the CURRENT technique object. Publish its resolved constant map.
void vcd_midhook(safetyhook::Context &ctx)
{
    const uintptr_t tech = (uintptr_t)ctx.edi;
    if (tech == 0) {
        return;
    }
    bool seen = false;
    for (uint32_t k = 0; k < g_tech_seen_n; k++) {
        if (g_tech_seen[k] == tech) {
            seen = true;
            break;
        }
    }
    const bool do_log = !seen && g_tech_seen_n < 32;
    if (do_log) {
        g_tech_seen[g_tech_seen_n++] = tech;
    }
    const uint32_t vcd_reg = *(volatile const uint32_t *)(tech + MC2_TECH_VCD_REG_OFF);
    const uint32_t vcd_count = *(volatile const uint32_t *)(tech + MC2_TECH_VCD_COUNT_OFF);
    const uint32_t vp_reg = *(volatile const uint32_t *)(tech + MC2_TECH_VP_REG_OFF);
    const uint32_t vp_count = *(volatile const uint32_t *)(tech + MC2_TECH_VP_COUNT_OFF);
    // Fail-safe against a stale/garbage technique pointer.
    const bool sane = (vcd_reg == REG_INVALID || vcd_reg < VS_ROWS) && vcd_count <= 8 &&
                      (vp_reg == REG_INVALID || vp_reg < VS_ROWS) && vp_count <= 4;
    if (!sane) {
        if (do_log) {
            MC2VR_LOG("view: technique %p implausible constant map (vcd c%u/%u, "
                      "ViewProj c%u/%u) — publish skipped",
                      (void *)tech, vcd_reg, vcd_count, vp_reg, vp_count);
        }
        return;
    }
    g_vcd_reg = vcd_reg;
    g_vcd_count = vcd_count;
    g_vp_reg = vp_reg;
    g_vp_count = vp_count;
    if (do_log) {
        MC2VR_LOG("view: technique %p: viewContextData c%u (count %u), ViewProj c%u (count %u)",
                  (void *)tech, vcd_reg, vcd_count, vp_reg, vp_count);
    }
}

RewriteMode parse_rewrite_mode(const char *value, bool *ok)
{
    *ok = true;
    if (strcmp(value, "off") == 0) {
        return RewriteMode::Off;
    }
    if (strcmp(value, "on") == 0) {
        return RewriteMode::On;
    }
    if (strcmp(value, "pulse") == 0) {
        return RewriteMode::Pulse;
    }
    *ok = false;
    return RewriteMode::Off;
}

// World-space pan distance for this frame (+x). `pulse` = ~2s sine at 60fps.
float rewrite_delta()
{
    if (g_mode == RewriteMode::Off) {
        return 0.0f;
    }
    if (g_mode == RewriteMode::Pulse) {
        return g_amp * sinf(6.2831853f * (float)(hooks::frame_count() % 120u) / 120.0f);
    }
    return g_amp;
}

} // namespace

bool set_view_row_rewrite(const char *value)
{
    bool ok;
    const RewriteMode mode = parse_rewrite_mode(value, &ok);
    if (!ok) {
        return false;
    }
    g_mode = mode;
    MC2VR_LOG("view: rewrite mode = %s", value);
    return true;
}

void set_view_row_amp(float amp)
{
    if (amp > 0.0f && amp <= 100.0f) {
        g_amp = amp;
        MC2VR_LOG("view: pan amplitude set to %g world units", (double)amp);
    } else {
        MC2VR_LOG("view: view_row_amp=%g out of range (0,100], keeping %g", (double)amp,
                  (double)g_amp);
    }
}

void set_main_rt_size(uint32_t w, uint32_t h)
{
    g_main_w = w;
    g_main_h = h;
    MC2VR_LOG("view: main scene RT size = %ux%u", w, h);
}

void on_set_render_target(uint32_t w, uint32_t h)
{
    g_rt_w = w;
    g_rt_h = h;
    for (uint32_t k = 0; k < g_rt_seen_n; k++) {
        if (g_rt_seen[k][0] == w && g_rt_seen[k][1] == h) {
            return;
        }
    }
    if (g_rt_seen_n < 16) {
        g_rt_seen[g_rt_seen_n][0] = w;
        g_rt_seen[g_rt_seen_n][1] = h;
        g_rt_seen_n++;
        MC2VR_LOG("view: new RT0 size %ux%u (%s)", w, h,
                  (g_main_w == 0 || (w == g_main_w && h == g_main_h))
                      ? "main pass — rewritten"
                      : "offscreen pass — not rewritten");
    }
}

// The rewrite. `viewContextData` block layout (proven from shader bytecode,
// tools/shader_disasm.py):
//   count 4: [VP row0..3]
//   count 5: [VP row0..3 | camPos (w==1)]
//   count 6: [VP row0..3 | camPos | extra row (a world-fixed plane — left alone)]
// Row-major, clip_i = dot(VP_row_i, worldpos). A camera pan by world-space
// D = (delta,0,0) is: camPos.x += delta; every VP row w -= row.x*delta
// (per-row — a uniform clip-w shift does not work, the divide scales it
// per-vertex). The projection is folded into the VP rows, so an asymmetric
// per-eye projection is an edit to the same rows.
const float *on_set_vs_constant(uint32_t start_register, const float *data,
                                uint32_t vec4_count)
{
    g_vs_calls++;
    if (!data || vec4_count == 0) {
        return data;
    }
    g_vs_vec4s += vec4_count;

    const float delta = pass_is_main() ? rewrite_delta() : 0.0f;
    if (delta == 0.0f || vec4_count > VS_ROWS) {
        return data;
    }
    const uint32_t vcd_reg = g_vcd_reg;
    const uint32_t vcd_count = g_vcd_count;
    const uint32_t vp_reg = g_vp_reg;
    const uint32_t vp_count = g_vp_count;
    const bool have_vcd = vcd_reg != REG_INVALID;
    const bool have_vp = vp_reg != REG_INVALID;
    if (!have_vcd && !have_vp) {
        return data;
    }

    // Rewrite a scratch copy: `data` may point straight into the game's
    // persistent per-view record, and editing it would re-apply the shift on
    // every draw that re-uploads it.
    static float scratch[VS_ROWS * 4];
    memcpy(scratch, data, vec4_count * 16);
    for (uint32_t i = 0; i < vec4_count; i++) {
        float *row = scratch + i * 4;
        const uint32_t reg = start_register + i;
        const uint32_t vcd_off = reg - vcd_reg;  // wraps when reg < vcd_reg
        const bool is_vp = (have_vcd && vcd_count >= 4 && vcd_off < 4) ||
                           (have_vp && reg >= vp_reg && reg < vp_reg + vp_count);
        const bool is_pos = have_vcd && vcd_count >= 5 && vcd_off == 4;
        if (is_vp && row[3] != 1.0f) {
            row[3] -= row[0] * delta;
            g_rows_rewritten++;
        } else if (is_pos && row[3] == 1.0f) {
            row[0] += delta;
            g_rows_rewritten++;
        }
    }
    return scratch;
}

void report_window()
{
    MC2VR_LOG("view: uploads calls=%llu vec4s=%llu | rewritten rows=%llu (mode=%s)",
              (unsigned long long)g_vs_calls, (unsigned long long)g_vs_vec4s,
              (unsigned long long)g_rows_rewritten,
              g_mode == RewriteMode::Pulse ? "pulse"
              : g_mode == RewriteMode::On  ? "on"
                                           : "off");
    g_vs_calls = 0;
    g_vs_vec4s = 0;
    g_rows_rewritten = 0;
}

void install()
{
    // Plaintext .text MidHook (same pattern as the M2.5/M3 MidHooks). Failure
    // is non-fatal: the map stays INVALID and the rewrite idles.
    auto mid = SafetyHookMid::create(reinterpret_cast<uint8_t *>(MC2_VCD_UPLOAD_CMP), vcd_midhook);
    if (!mid) {
        MC2VR_LOG("view: upload-gate MidHook install failed @ %p (error %u); "
                  "rewrite stays idle",
                  (void *)MC2_VCD_UPLOAD_CMP, (unsigned)mid.error().type);
        return;
    }
    g_vcd_mid = std::move(*mid);
    MC2VR_LOG("view: installed upload-gate MidHook @ %p", (void *)MC2_VCD_UPLOAD_CMP);
}

} // namespace mc2vr::view
