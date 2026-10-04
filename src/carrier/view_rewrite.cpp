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
constexpr float DEFAULT_IPD = 0.065f;   // world units (metres), average adult IPD
constexpr float DEFAULT_HOLD = 2.0f;    // stereo eye A/B hold, seconds

enum class RewriteMode { Off, On, Pulse, Stereo };
RewriteMode g_mode = RewriteMode::Off;
float g_amp = DEFAULT_AMP;
float g_ipd = DEFAULT_IPD;
float g_hold_s = DEFAULT_HOLD;
// Per-eye asymmetric-projection centre shift, NDC units (S2 step 1: "an edit
// to the same rows"). 0 = disabled until S4 supplies real per-eye tan angles;
// the row.w scaling is |row.xyz| = the projection coefficient, and the sign
// convention is to be validated against the HMD runtime then.
float g_asym_x = 0.0f, g_asym_y = 0.0f;

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

// ---- stereo eye state -----------------------------------------------------------
// Camera right axis in world space, derived from the raw (pre-rewrite) VP
// row0 upload: row0.xyz = P00 * right (row-major, clip.x = dot(row0, p), the
// view row0 is the camera right axis), so normalize(row0.xyz) is `right`.
// The cache is at most one frame old — a sub-degree direction error on a
// 0.032-unit offset, and the first technique's upload of each frame refreshes
// it for the rest of the frame.
float g_right[3] = {0.0f, 0.0f, 0.0f};
bool g_right_valid = false;
int g_stereo_eye = +1;  // +1 right / -1 left; A/B alternating until S2c
uint64_t g_stereo_flip_ms = 0;
int g_pass_eye = 0;    // S2c-2 per-pass override (eye_replay.cpp); 0 = hold timer

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
    if (strcmp(value, "stereo") == 0) {
        return RewriteMode::Stereo;
    }
    *ok = false;
    return RewriteMode::Off;
}

// World-space pan for this frame. `pulse` = ~2s sine at 60fps. `stereo`
// alternates the eye sign every view_stereo_hold seconds (A/B verification
// until the S2c replay supplies real per-eye passes) and pans along the
// camera right axis by ±view_ipd/2. `asym` carries the per-eye NDC
// projection-centre shift (sign follows the eye).
void rewrite_delta(float d[3], float asym[2])
{
    d[0] = d[1] = d[2] = 0.0f;
    asym[0] = asym[1] = 0.0f;
    if (g_mode == RewriteMode::Off) {
        return;
    }
    if (g_mode == RewriteMode::Pulse) {
        d[0] = g_amp * sinf(6.2831853f * (float)(hooks::frame_count() % 120u) / 120.0f);
        return;
    }
    if (g_mode == RewriteMode::On) {
        d[0] = g_amp;
        return;
    }
    // Stereo. While a per-pass override is active (S2c-2), it wins over the
    // hold timer and no flip is logged or applied.
    const bool use_pass_eye = g_pass_eye != 0;
    if (!use_pass_eye) {
        const uint64_t now = GetTickCount64();
        if (g_stereo_flip_ms == 0) {
            g_stereo_flip_ms = now;
        } else if ((float)(now - g_stereo_flip_ms) >= g_hold_s * 1000.0f) {
            g_stereo_flip_ms = now;
            g_stereo_eye = -g_stereo_eye;
            MC2VR_LOG("view: stereo eye -> %s (right=[%.4f %.4f %.4f], off=%.4f)",
                      g_stereo_eye > 0 ? "RIGHT" : "LEFT", (double)g_right[0],
                      (double)g_right[1], (double)g_right[2],
                      (double)(g_ipd * 0.5f));
        }
    }
    if (!g_right_valid) {
        return;  // only until the first VP row0 upload lands (start of frame 1)
    }
    const int eye = use_pass_eye ? g_pass_eye : g_stereo_eye;
    const float half = g_ipd * 0.5f * (float)eye;
    d[0] = g_right[0] * half;
    d[1] = g_right[1] * half;
    d[2] = g_right[2] * half;
    asym[0] = g_asym_x * (float)eye;
    asym[1] = g_asym_y * (float)eye;
}

bool have_vp_or_vcd_regs()
{
    return g_vcd_reg != REG_INVALID || g_vp_reg != REG_INVALID;
}

// Scan one RAW upload for the VP row0 (first row of viewContextData/ViewProj)
// and cache normalize(row0.xyz) as the world-space camera right axis. Needed
// for the stereo cold-start (the rewrite still early-outs on a zero delta, so
// the in-loop cache would never fire); once running, the in-loop refresh in
// on_set_vs_constant keeps it current every frame.
void cache_right_axis(uint32_t start_register, const float *data, uint32_t vec4_count)
{
    const uint32_t vcd_reg = g_vcd_reg;
    const uint32_t vcd_count = g_vcd_count;
    const uint32_t vp_reg = g_vp_reg;
    for (uint32_t i = 0; i < vec4_count; i++) {
        const uint32_t reg = start_register + i;
        const uint32_t vcd_off = reg - vcd_reg;  // wraps when reg < vcd_reg
        const bool is_row0 = (vcd_reg != REG_INVALID && vcd_count >= 4 && vcd_off == 0) ||
                             (vp_reg != REG_INVALID && reg == vp_reg);
        if (!is_row0) {
            continue;
        }
        // row0.xyz = P00 * right (view row0 = camera right axis), so the
        // normalized xyz IS the world right direction.
        const float *r = data + i * 4;
        const float n2 = r[0] * r[0] + r[1] * r[1] + r[2] * r[2];
        if (n2 > 1e-20f) {
            const float inv = 1.0f / sqrtf(n2);
            g_right[0] = r[0] * inv;
            g_right[1] = r[1] * inv;
            g_right[2] = r[2] * inv;
            g_right_valid = true;
        }
        return;  // at most one row0 per upload
    }
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

void set_view_ipd(float ipd)
{
    if (ipd > 0.0f && ipd <= 1.0f) {
        g_ipd = ipd;
        MC2VR_LOG("view: stereo IPD set to %g world units (per-eye offset %g)",
                  (double)ipd, (double)(ipd * 0.5f));
    } else {
        MC2VR_LOG("view: view_ipd=%g out of range (0,1], keeping %g", (double)ipd,
                  (double)g_ipd);
    }
}

void set_view_stereo_hold(float seconds)
{
    if (seconds >= 0.1f && seconds <= 60.0f) {
        g_hold_s = seconds;
        MC2VR_LOG("view: stereo eye A/B hold set to %g s", (double)seconds);
    } else {
        MC2VR_LOG("view: view_stereo_hold=%g out of range [0.1,60], keeping %g",
                  (double)seconds, (double)g_hold_s);
    }
}

void set_view_asym(float x, float y)
{
    g_asym_x = x;
    g_asym_y = y;
    MC2VR_LOG("view: asymmetric projection centre shift = (%g, %g) NDC "
              "(sign convention to be validated against the HMD runtime, S4)",
              (double)x, (double)y);
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
// Row-major, clip_i = dot(VP_row_i, worldpos). A camera pan by world-space D is:
// camPos.xyz += D; every VP row w -= dot(row.xyz, D) (per-row — a uniform
// clip-w shift does not work, the divide scales it per-vertex). The
// projection is folded into the VP rows, so an asymmetric per-eye projection
// is an edit to the same rows: the NDC centre shift e_k lands in row_k.w as
// |row_k.xyz| * e_k (row_k.xyz = P_kk * basis_k).
const float *on_set_vs_constant(uint32_t start_register, const float *data,
                                uint32_t vec4_count)
{
    g_vs_calls++;
    if (!data || vec4_count == 0) {
        return data;
    }
    g_vs_vec4s += vec4_count;

    float d[3] = {0.0f, 0.0f, 0.0f};
    float asym[2] = {0.0f, 0.0f};
    if (pass_is_main()) {
        rewrite_delta(d, asym);
    }
    // Stereo cold-start: the right-axis cache must fill from the raw uploads
    // even while the pan is still zero (first frame), or it never would.
    // Main pass only — offscreen passes (shadow/reflection) upload their own
    // viewContextData with the LIGHT's basis, which must not seed the cache.
    if (g_mode == RewriteMode::Stereo && !g_right_valid && pass_is_main() &&
        have_vp_or_vcd_regs()) {
        cache_right_axis(start_register, data, vec4_count);
    }
    if ((d[0] == 0.0f && d[1] == 0.0f && d[2] == 0.0f && asym[0] == 0.0f &&
         asym[1] == 0.0f) ||
        vec4_count > VS_ROWS) {
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
        const float *raw = data + i * 4;
        const uint32_t reg = start_register + i;
        const uint32_t vcd_off = reg - vcd_reg;  // wraps when reg < vcd_reg
        uint32_t vp_idx = 0xffffffffu;
        if (have_vcd && vcd_count >= 4 && vcd_off < 4) {
            vp_idx = vcd_off;
        } else if (have_vp && reg >= vp_reg && reg < vp_reg + vp_count) {
            vp_idx = reg - vp_reg;
        }
        const bool is_pos = have_vcd && vcd_count >= 5 && vcd_off == 4;
        if (vp_idx == 0) {
            // Refresh the camera right axis from the RAW upload (pre-rewrite —
            // caching the shifted row would accumulate the offset per frame).
            // Fires every frame; keeps up with camera rotation within a frame.
            const float n2 = raw[0] * raw[0] + raw[1] * raw[1] + raw[2] * raw[2];
            if (n2 > 1e-20f) {
                const float inv = 1.0f / sqrtf(n2);
                g_right[0] = raw[0] * inv;
                g_right[1] = raw[1] * inv;
                g_right[2] = raw[2] * inv;
                g_right_valid = true;
            }
        }
        if (vp_idx != 0xffffffffu && row[3] != 1.0f) {
            row[3] -= row[0] * d[0] + row[1] * d[1] + row[2] * d[2];
            // Asymmetric per-eye projection centre (stereo mode, S2 step 1):
            // row_k.xyz = P_kk * basis_k, so |row_k.xyz| is the projection
            // coefficient that scales the NDC shift into row_k.w. Sign
            // convention to be validated against the HMD runtime (S4).
            if (asym[0] != 0.0f && vp_idx == 0) {
                row[3] += sqrtf(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]) *
                          asym[0];
            } else if (asym[1] != 0.0f && vp_idx == 1) {
                row[3] += sqrtf(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]) *
                          asym[1];
            }
            g_rows_rewritten++;
        } else if (is_pos && row[3] == 1.0f) {
            row[0] += d[0];
            row[1] += d[1];
            row[2] += d[2];
            g_rows_rewritten++;
        }
    }
    return scratch;
}

void report_window()
{
    const char *mode = g_mode == RewriteMode::Pulse ? "pulse"
                       : g_mode == RewriteMode::On    ? "on"
                       : g_mode == RewriteMode::Stereo ? "stereo"
                                                       : "off";
    MC2VR_LOG("view: uploads calls=%llu vec4s=%llu | rewritten rows=%llu (mode=%s%s)",
              (unsigned long long)g_vs_calls, (unsigned long long)g_vs_vec4s,
              (unsigned long long)g_rows_rewritten, mode,
              g_mode == RewriteMode::Stereo
                  ? (g_stereo_eye > 0 ? ", eye=R" : ", eye=L")
                  : "");
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

void set_pass_eye(int sign)
{
    g_pass_eye = sign;
}

} // namespace mc2vr::view
