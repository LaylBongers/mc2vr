// S2 channel module — see s1_probe.hpp for scope. S1 hunt instrumentation
// removed (S1 complete, 2026-10-02 — evidence distilled into
// docs/stereo_design.md); this file keeps only the SetVertexShaderConstantF
// channel + the rewrites.

#include "s1_probe.hpp"

#include <safetyhook.hpp>

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "game_addresses.h"
#include "hooks.hpp"
#include "log.hpp"

namespace mc2vr::s1 {

namespace {

constexpr uint32_t VS_ROWS = 256;  // vs_2_0 max registers
constexpr float PATCH_DELTA = 4.0f;
// viewContextData pan amplitude (mc2vr.conf view_row_amp, default 4.0 —
// the unmistakable verification value; use ~0.05 for game-scale checks,
// 0.032 = IPD scale).
float g_vrow_amp = PATCH_DELTA;



// ---- ambient rewrite modes (mc2vr.conf) -------------------------------------
enum class RewriteMode { Off, On, Pulse };
// gpu_boundary_rewrite: w==1.0 world-position rows (verified visible — S1
// runs 16–17; moves EFFECTS, not the camera).
RewriteMode g_ambient_rewrite = RewriteMode::Off;
// view_row_rewrite: the exact-register viewContextData rewrite — the camera
// pan, VALIDATED run 22 (stereo_design.md §S2). Separate key so the camera
// pan can run with the ambient effects rewrite disabled.
RewriteMode g_view_rewrite = RewriteMode::Off;

// ---- VS register cache (the S2a identification input) ------------------------
// Last uploaded 4-float row per register + validity (pre-rewrite values).
float g_vs_rows[VS_ROWS][4];
uint8_t g_vs_valid[VS_ROWS];
uint64_t g_vs_frame_key = UINT64_MAX;

// ---- live technique constant map (published by the VCD MidHook) -------------
// Run-21 lesson: register bases slide PER TECHNIQUE, and different techniques
// reuse the same register numbers — value-shape heuristics conflate them
// (objects panned in different directions / not at all). Run-22: the MidHook
// at the upload gate (MC2_VCD_UPLOAD_CMP, EDI = current technique) publishes
// the EXACT (reg, count) pairs the engine's own resolver stored
// (Technique_ResolveConstantRegisters plate):
//   [edi+0xd4] viewContextData reg      [edi+0xd8] count (4 or 5)
//   [edi+0xdc] viewContextData.ViewProj reg  [edi+0xe0] count
// Same thread, fires immediately before the wrapper -> device VmtHook call
// chain, so on_set_vs_constant always sees the map for the CURRENT upload.
constexpr uint32_t VCD_REG_INVALID = 0xffffffffu;
volatile uint32_t g_tech_vcd_reg = VCD_REG_INVALID;
volatile uint32_t g_tech_vcd_count = 0;
volatile uint32_t g_tech_vp_reg = VCD_REG_INVALID;
volatile uint32_t g_tech_vp_count = 0;
uintptr_t g_tech_seen[32];    // distinct technique objects (log once each)
uint32_t g_tech_seen_n = 0;
SafetyHookMid g_vcd_mid;

// ---- window report aggregates -------------------------------------------------
uint64_t g_vs_calls = 0, g_vs_vec4s = 0;
uint64_t g_owin_rewrites = 0;
uint32_t g_owin_log_regs[24]; // one-shot per distinct rewritten register
uint32_t g_owin_log_n = 0;
uint64_t g_vrow_rewrites = 0;
uint32_t g_vrow_log_regs[24];
uint32_t g_vrow_log_n = 0;

// ---- viewContextData block shapes (S2a SOLVED — stereo_design.md §S2) ----
// Block = [camPos (w==1.0) | VP row0..row3] (count 5) or VP only (count 4);
// row-major, clip_i = dot(row_i, worldpos). VP rows are view rows scaled by
// ~1 projection terms: xyz rotation-like + unit norm, translation in w.
// VP row3 = view row2 = the clip.w source. With the EXACT register map from
// the MidHook these are only sanity checks (fail-safe against a stale/garbage
// technique pointer), not the matcher.

// ---- pass gate (RT0 size) ----------------------------------------------------
uint32_t g_main_w = 0, g_main_h = 0;
uint32_t g_rt_w = 0, g_rt_h = 0;
uint32_t g_rt_seen[16][2];  // distinct RT0 sizes (log once each)
uint32_t g_rt_seen_n = 0;

bool pass_is_main()
{
    // Unknown main size (params not yet read): don't gate.
    return g_main_w == 0 || (g_rt_w == g_main_w && g_rt_h == g_main_h);
}

bool pos_row_shape(const float *r)
{
    return r[3] == 1.0f;
}

// MidHook @ MC2_VCD_UPLOAD_CMP (cmp [edi+0xd8],0 at the upload gate):
// EDI = the CURRENT technique object. Publish its resolved constant map.
void vcd_midhook(safetyhook::Context &ctx)
{
    const uintptr_t tech = (uintptr_t)ctx.edi;
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
    if (tech == 0) {
        return;
    }
    const uint32_t vcd_reg = *(volatile const uint32_t *)
        (tech + MC2_TECH_VCD_REG_OFF);
    const uint32_t vcd_count = *(volatile const uint32_t *)
        (tech + MC2_TECH_VCD_COUNT_OFF);
    const uint32_t vp_reg = *(volatile const uint32_t *)
        (tech + MC2_TECH_VP_REG_OFF);
    const uint32_t vp_count = *(volatile const uint32_t *)
        (tech + MC2_TECH_VP_COUNT_OFF);
    const bool sane = (vcd_reg == VCD_REG_INVALID || vcd_reg < VS_ROWS) &&
                      vcd_count <= 8 &&
                      (vp_reg == VCD_REG_INVALID || vp_reg < VS_ROWS) &&
                      vp_count <= 4;
    if (!sane) {
        if (do_log) {
            MC2VR_LOG("S1 vrow: technique %p implausible constant map "
                      "(vcd c%u/%u, ViewProj c%u/%u) — publish skipped",
                      (void *)tech, vcd_reg, vcd_count, vp_reg, vp_count);
        }
        return;
    }
    g_tech_vcd_reg = vcd_reg;
    g_tech_vcd_count = vcd_count;
    g_tech_vp_reg = vp_reg;
    g_tech_vp_count = vp_count;
    if (do_log)
        MC2VR_LOG("S1 vrow: technique %p: viewContextData c%u (count %u), "
                  "ViewProj c%u (count %u) — exact registers published",
                  (void *)tech, vcd_reg, vcd_count, vp_reg, vp_count);
}

// One-shot per-register logs (first rewrite only — the identification
// input for S2a; echoed by tools/analyze_dumps.py).
bool already_logged(const uint32_t *regs, uint32_t n, uint32_t reg)
{
    for (uint32_t k = 0; k < n; k++) {
        if (regs[k] == reg) {
            return true;
        }
    }
    return false;
}

void log_once_owin(uint32_t reg, const float *row, float delta)
{
    if (already_logged(g_owin_log_regs, g_owin_log_n, reg) ||
        g_owin_log_n >= 24) {
        return;
    }
    g_owin_log_regs[g_owin_log_n++] = reg;
    MC2VR_LOG("S1 owin: rewriting c%u (was (%g,%g,%g,%g)) — "
              "world-position row; moves effects, not the camera",
              reg, (double)(row[0] - delta), (double)row[1],
              (double)row[2], (double)row[3]);
}

void log_once_vrow(uint32_t reg, const float *pos, float delta)
{
    if (already_logged(g_vrow_log_regs, g_vrow_log_n, reg) ||
        g_vrow_log_n >= 24) {
        return;
    }
    g_vrow_log_regs[g_vrow_log_n++] = reg;
    MC2VR_LOG("S1 vrow: viewContextData block at c%u (pos was (%g,%g,%g,1))"
              " — shifting pos.x and VP rows 0-3 (w -= x*delta), delta=%g "
              "(S2a-solved block layout; stereo_design.md §S2)",
              reg, (double)(pos[0] - delta), (double)pos[1], (double)pos[2],
              (double)delta);
}

// Count-6 blocks carry an extra row after camPos whose role is unclassified
// (shader 0x1de598: `dp4 r1, r0, c22`). One-shot log of it beside VP row0 so
// a world-fixed row (no shift needed) can be told from a view-derived one.
bool g_extra_logged = false;
void log_extra_row(uint32_t reg, const float *vp0, const float *row, bool ok)
{
    if (g_extra_logged || !ok) {
        return;
    }
    g_extra_logged = true;
    MC2VR_LOG("S1 vrow: count-6 extra row c%u = (%g,%g,%g,%g); VP row0 = "
              "(%g,%g,%g,%g) — left unshifted",
              reg, (double)row[0], (double)row[1], (double)row[2],
              (double)row[3], (double)vp0[0], (double)vp0[1], (double)vp0[2],
              (double)vp0[3]);
}

// Diagnostic removed with the run-21 prev-frame oracle (shape matching is
// obsolete — exact registers come from the MidHook).

} // namespace

void set_main_rt_size(uint32_t w, uint32_t h)
{
    g_main_w = w;
    g_main_h = h;
    MC2VR_LOG("S1 pass gate: main scene RT size = %ux%u", w, h);
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
        MC2VR_LOG("S1 pass gate: new RT0 size %ux%u (%s)", w, h,
                  (g_main_w == 0 || (w == g_main_w && h == g_main_h))
                      ? "main — view rewrite applies"
                      : "off-screen pass — NOT rewritten");
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

float rewrite_delta(RewriteMode mode, float amp)
{
    if (mode == RewriteMode::Off) {
        return 0.0f;
    }
    if (mode == RewriteMode::Pulse) {
        // ~2s sine drift at 60fps — unmistakable, self-reversing.
        return amp * sinf(6.2831853f * (float)(hooks::frame_count() % 120u) / 120.0f);
    }
    return amp;
}

bool set_ambient_rewrite(const char *value)
{
    bool ok;
    RewriteMode mode = parse_rewrite_mode(value, &ok);
    if (!ok) {
        return false;
    }
    g_ambient_rewrite = mode;
    MC2VR_LOG("S1 owin: ambient GPU-boundary rewrite mode = %s%s", value,
              mode != RewriteMode::Off
                  ? " (every world-position VS constant row w==1.0, |x|>5 "
                    "gets shifted IN THE UPLOAD — moves EFFECTS, not the "
                    "camera)"
                  : " (pass-through)");
    return true;
}

bool set_view_row_rewrite(const char *value)
{
    bool ok;
    RewriteMode mode = parse_rewrite_mode(value, &ok);
    if (!ok) {
        return false;
    }
    g_view_rewrite = mode;
    MC2VR_LOG("S1 vrow: viewContextData rewrite mode = %s%s", value,
              mode != RewriteMode::Off
                  ? " (EXACT registers via upload-gate MidHook: pos.x += delta,"
                    " every VP row w -= row.x*delta = consistent pan; run 22)"
                  : " (pass-through)");
    return true;
}

void set_view_row_amp(float amp)
{
    if (amp > 0.0f && amp <= 100.0f) {
        g_vrow_amp = amp;
        MC2VR_LOG("S1 vrow: pan amplitude set to %g world units", (double)amp);
    } else {
        MC2VR_LOG("S1 vrow: view_row_amp=%g out of range (0,100], keeping %g",
                  (double)amp, (double)g_vrow_amp);
    }
}

const float *on_set_vs_constant(uint32_t start_register, const float *data,
                                uint32_t vec4_count)
{
    g_vs_calls++;
    if (!data || vec4_count == 0) {
        return data;
    }
    g_vs_vec4s += vec4_count;
    const uint64_t frame = hooks::frame_count();
    if (frame != g_vs_frame_key) {
        g_vs_frame_key = frame;
    }

    // Register cache: rows exactly as uploaded (pre-rewrite).
    if (start_register < VS_ROWS) {
        const uint32_t n = vec4_count < VS_ROWS - start_register
                               ? vec4_count
                               : VS_ROWS - start_register;
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t r = start_register + i;
            memcpy(g_vs_rows[r], data + i * 4, 16);
            g_vs_valid[r] = 1;
        }
    }

    // Rewrites happen on a scratch COPY (returned to the caller for the
    // driver call): `data` may point straight into the game's persistent
    // per-view record, and editing that in place would re-apply the shift on
    // every draw that re-uploads it.
    const float delta_owin = rewrite_delta(g_ambient_rewrite, PATCH_DELTA);
    const float delta_vrow =
        pass_is_main() ? rewrite_delta(g_view_rewrite, g_vrow_amp) : 0.0f;
    if (delta_owin == 0.0f && delta_vrow == 0.0f) {
        return data;
    }
    static float scratch[VS_ROWS * 4];
    if (vec4_count > VS_ROWS) {
        return data;  // not a real constant upload; pass through
    }
    memcpy(scratch, data, vec4_count * 16);
    float *rows = scratch;

    // Ambient rewrite (S1 runs 16-17): world-position rows (w == 1.0,
    // |x| > 5). Moves EFFECTS, not the camera.
    if (delta_owin != 0.0f) {
        for (uint32_t i = 0; i < vec4_count; i++) {
            float *row = rows + i * 4;
            if (row[3] == 1.0f && (row[0] > 5.0f || row[0] < -5.0f)) {
                row[0] += delta_owin;
                g_owin_rewrites++;
                log_once_owin(start_register + i, row, delta_owin);
            }
        }
    }
    if (delta_vrow == 0.0f) {
        return scratch;
    }

    // viewContextData rewrite, EXACT-REGISTER form. Block layout (proven
    // from shader bytecode, stereo_design.md §S2): VP rows 0..3 FIRST, then
    // camPos (count >= 5), then a count-6 extra row (left alone). Register
    // map comes from the upload-gate MidHook. Consistent pan by
    // D=(delta,0,0): VP rows w -= row.x*delta; camPos x += delta.
    const uint32_t vcd_reg = g_tech_vcd_reg;
    const uint32_t vcd_count = g_tech_vcd_count;
    const uint32_t vp_reg = g_tech_vp_reg;
    const uint32_t vp_count = g_tech_vp_count;
    for (uint32_t i = 0; i < vec4_count; i++) {
        float *row = rows + i * 4;
        const uint32_t reg = start_register + i;
        if (reg >= VS_ROWS) {
            break;
        }
        const bool have_vcd = vcd_reg != VCD_REG_INVALID;
        const uint32_t vcd_off = reg - vcd_reg;  // wraps when reg < vcd_reg
        const bool is_vp =
            (have_vcd && vcd_count >= 4 && vcd_off < 4) ||
            (vp_reg != VCD_REG_INVALID && reg >= vp_reg &&
             reg < vp_reg + vp_count);
        const bool is_pos = have_vcd && vcd_count >= 5 && vcd_off == 4;
        if (is_vp && row[3] != 1.0f) {
            row[3] -= row[0] * delta_vrow;
            g_vrow_rewrites++;
        } else if (is_pos && pos_row_shape(row)) {
            row[0] += delta_vrow;
            g_vrow_rewrites++;
            log_once_vrow(reg, row, delta_vrow);
        } else if (have_vcd && vcd_count >= 6 && vcd_off == 5) {
            log_extra_row(reg, g_vs_rows[vcd_reg], row, g_vs_valid[vcd_reg] != 0);
        }
    }
    return scratch;
}

void report_window()
{
    MC2VR_LOG("S1 vs: calls=%llu vec4s=%llu | owin rewrites=%llu (mode=%s) "
              "| vrow rows=%llu (mode=%s)",
              (unsigned long long)g_vs_calls, (unsigned long long)g_vs_vec4s,
              (unsigned long long)g_owin_rewrites,
              g_ambient_rewrite == RewriteMode::Pulse ? "pulse"
              : g_ambient_rewrite == RewriteMode::On ? "on" : "off",
              (unsigned long long)g_vrow_rewrites,
              g_view_rewrite == RewriteMode::Pulse ? "pulse"
              : g_view_rewrite == RewriteMode::On ? "on" : "off");
    g_vs_calls = 0;
    g_vs_vec4s = 0;
    g_owin_rewrites = 0;
    g_vrow_rewrites = 0;
}

void install()
{
    // Run 22: publish the current technique's exact viewContextData register
    // map at the upload gate (plaintext .text MidHook, same pattern as the
    // M2.5/M3 MidHooks). Install failure is non-fatal: the rewrite just
    // stays idle (vcd_reg stays INVALID) — no wild shifts.
    auto mid = SafetyHookMid::create(
        reinterpret_cast<uint8_t *>(MC2_VCD_UPLOAD_CMP), vcd_midhook);
    if (!mid) {
        MC2VR_LOG("S1 vrow: upload-gate MidHook install failed @ %p "
                  "(error %u); viewContextData rewrite stays idle",
                  (void *)MC2_VCD_UPLOAD_CMP, (unsigned)mid.error().type);
    } else {
        g_vcd_mid = std::move(*mid);
        MC2VR_LOG("S1 vrow: installed upload-gate MidHook @ %p (publishes "
                  "the technique's exact viewContextData register map)",
                  (void *)MC2_VCD_UPLOAD_CMP);
    }

    MC2VR_LOG("S1: active channel: SetVertexShaderConstantF (device VmtHook "
              "slot 94) + ambient rewrite (gpu_boundary_rewrite) + "
              "exact-register viewContextData rewrite (view_row_rewrite) "
              "— stereo_design.md §S2 (run 22)");
}

} // namespace mc2vr::s1
