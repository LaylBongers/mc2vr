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
#include "ipc.hpp"
#include "log.hpp"
#include "vec_math.hpp"
#include "vp_camera.hpp"

namespace mc2vr::view {

namespace {

using math::Vec3;

constexpr uint32_t VS_ROWS = 256;  // vs_3_0 float constant registers (plus headroom)
constexpr float DEFAULT_AMP = 4.0f;
constexpr float DEFAULT_IPD = 0.065f;   // world units (metres), average adult IPD
constexpr float DEFAULT_HOLD = 2.0f;    // stereo eye A/B hold, seconds

// Hmd = S4-4 full VP replacement from the HMD pose (see hmd_rewrite below);
// HmdIdentity = the same decompose/rebuild with the game's OWN camera and
// projection — output must equal input (self-check of the decomposition).
enum class RewriteMode { Off, On, Pulse, Stereo, Hmd, HmdIdentity };
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
// E1 prep (docs/stereo_improvements_plan.md): viewContext RECORD stats from
// the gate ([esp+0x18] = the pass's record pointer). Each distinct record VA
// is logged once with the table base + computed index; per-window main/off
// upload counts then identify the world view's record by dominance. 32 slots
// covers all 11-ish indices x both double-buffer bases seen so far.
struct RecStats {
    uintptr_t va;
    uint64_t main_hits;
    uint64_t off_hits;
};
RecStats g_rec_seen[32];
uint32_t g_rec_seen_n = 0;
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
Vec3 g_right;
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
    // Record discovery (E1 prep, docs/stereo_improvements_plan.md): [esp+0x18]
    // at the gate is this pass's viewContext record pointer (game_addresses.h).
    // Each distinct record logs once with the live table base + index; every
    // hit also counts toward the per-window main/offscreen tally so the world
    // view's record identifies by upload dominance (all 24 views are walked
    // uniformly, so walk counts cannot discriminate — 2026-10-06 run 1).
    const uintptr_t rec =
        *(const uintptr_t *)(uintptr_t)(ctx.esp + MC2_VCD_GATE_REC_SLOT);
    uint32_t slot = g_rec_seen_n;
    for (uint32_t k = 0; k < g_rec_seen_n; k++) {
        if (g_rec_seen[k].va == rec) {
            slot = k;
            break;
        }
    }
    if (slot == g_rec_seen_n) {
        if (g_rec_seen_n >= sizeof(g_rec_seen) / sizeof(g_rec_seen[0])) {
            return;  // table full — count nothing further (diagnostic only)
        }
        const uintptr_t base = *(const uintptr_t *)MC2_G_VIEWCONTEXTTABLE;
        const uintptr_t off = rec - base;
        // Current buffer only (32 slots); the other buffer's records differ by
        // 0xe00 and would wrap the modulo check anyway.
        const bool plausible =
            rec != 0 && base != 0 && (off % MC2_VIEWCONTEXT_STRIDE) == 0 &&
            off < MC2_VIEWCONTEXT_BUFFERSZ;
        if (plausible) {
            MC2VR_LOG("view: record %p = table %p + idx %u, pass %s (RT %ux%u)",
                      (void *)rec, (void *)base,
                      (unsigned)(off / MC2_VIEWCONTEXT_STRIDE),
                      pass_is_main() ? "main" : "offscreen", g_rt_w, g_rt_h);
        } else {
            MC2VR_LOG("view: record slot %p implausible (table %p) — kept for diagnosis",
                      (void *)rec, (void *)base);
        }
        g_rec_seen[g_rec_seen_n++] = {rec, 0, 0};
    }
    if (pass_is_main()) {
        g_rec_seen[slot].main_hits++;
    } else {
        g_rec_seen[slot].off_hits++;
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
    if (strcmp(value, "hmd") == 0) {
        return RewriteMode::Hmd;
    }
    if (strcmp(value, "hmd_identity") == 0) {
        return RewriteMode::HmdIdentity;
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
                      g_stereo_eye > 0 ? "RIGHT" : "LEFT", (double)g_right.x,
                      (double)g_right.y, (double)g_right.z,
                      (double)(g_ipd * 0.5f));
        }
    }
    if (!g_right_valid) {
        return;  // only until the first VP row0 upload lands (start of frame 1)
    }
    const int eye = use_pass_eye ? g_pass_eye : g_stereo_eye;
    const float half = g_ipd * 0.5f * (float)eye;
    math::store3(d, g_right * half);
    asym[0] = g_asym_x * (float)eye;
    asym[1] = g_asym_y * (float)eye;
}

bool have_vp_or_vcd_regs()
{
    return g_vcd_reg != REG_INVALID || g_vp_reg != REG_INVALID;
}

// row0.xyz = P00 * right (the view row0 is the camera right axis), so the
// normalized xyz IS the world right direction. Takes the RAW (pre-rewrite)
// row — caching a shifted row would accumulate the offset per frame.
void refresh_right_axis(const float *row0)
{
    if (math::normalize(math::load3(row0), &g_right, 1e-10f)) {
        g_right_valid = true;
    }
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
        refresh_right_axis(data + i * 4);
        return;  // at most one row0 per upload
    }
}


// ---- S4-4: HMD camera replacement -------------------------------------------------
// The VP block is fully decomposable (D3D clip = [a x_v + c z_v, b y_v + d z_v,
// A z_v + B, z_v] with x_v/y_v/z_v = dot(R/U/F, p - C), R/U/F orthonormal):
//   row0 = a R + c F        row1 = b U + d F
//   row2 = A F              row3 = F          (xyz; w = -dot(xyz, C), row2.w += B)
// so the game camera (C, R, U, F), its projection terms (a, b, c, d) and its
// depth terms (A, B) all fall out of the four rows. The HMD replaces the
// camera ORIENTATION and POSITION (game camera = body; HMD pose = offset on
// it) and the projection terms (OpenXR FOV); A,B are kept so depth / fog /
// soft-particle behaviour is unchanged.
struct HmdSnapshot {
    bool valid = false;
    uint32_t id = 0;  // pose id (hostFrame+1) the carrier tags frames with
    vpcam::EyePose eye[2];
};
HmdSnapshot g_hmd;
float g_world_scale = 1.0f;  // game world units per metre (UNVERIFIED default)

// Per-pass cache of the rebuilt camera position, for uploads that carry only
// the camPos row (VP rows arrived in an earlier call).
Vec3 g_pw;
bool g_pw_valid = false;

uint64_t g_hmd_blocks = 0, g_hmd_split = 0, g_hmd_decomp_fail = 0, g_hmd_cam_only = 0;
float g_hmd_resid_max = 0.0f;  // max |rebuilt - raw| over the window (identity self-check)
uint32_t g_hmd_split_logged = 0, g_hmd_fail_logged = 0;

// Host-published eye (IPC layout) -> pure-math eye.
vpcam::EyePose to_eye_pose(const Mc2IpcEyePose &e)
{
    return {{e.pos.x, e.pos.y, e.pos.z},
            {e.rot.x, e.rot.y, e.rot.z, e.rot.w},
            e.fov.left, e.fov.right, e.fov.up, e.fov.down};
}

// A published eye is usable if its quaternion is unit and its FOV non-degenerate.
bool eye_is_sane(const Mc2IpcEyePose &e)
{
    const float n = math::norm_sq({e.rot.x, e.rot.y, e.rot.z, e.rot.w});
    return n >= 0.98f && n <= 1.02f && e.fov.right > e.fov.left && e.fov.up > e.fov.down;
}

const float *hmd_rewrite(uint32_t start_register, const float *data, uint32_t vec4_count)
{
    const bool identity = g_mode == RewriteMode::HmdIdentity;
    const int eye = g_pass_eye > 0 ? 1 : 0;
    if (!identity && (!g_hmd.valid || g_pass_eye == 0)) {
        return data;
    }
    const uint32_t vcd_reg = g_vcd_reg, vcd_count = g_vcd_count;
    uint32_t base = REG_INVALID;
    bool has_cam = false;
    if (vcd_reg != REG_INVALID && vcd_count >= 4) {
        base = vcd_reg;
        has_cam = vcd_count >= 5;
    } else if (g_vp_reg != REG_INVALID && g_vp_count >= 4) {
        base = g_vp_reg;
    }
    if (base == REG_INVALID || vec4_count > VS_ROWS) {
        return data;
    }
    const uint32_t end = start_register + vec4_count;
    const bool any_vp = start_register < base + 4 && end > base;
    const bool all_vp = start_register <= base && end >= base + 4;
    const bool cam_in = has_cam && start_register <= base + 4 && end >= base + 5;
    if (!any_vp && !cam_in) {
        return data;
    }
    static float scratch[VS_ROWS * 4];
    memcpy(scratch, data, vec4_count * 16);
    float *camrow = cam_in ? scratch + (base + 4 - start_register) * 4 : nullptr;
    if (!all_vp) {
        if (any_vp) {
            // VP block split across uploads: needs gathering — counted so the
            // live run says whether this ever happens.
            g_hmd_split++;
            if (g_hmd_split_logged++ < 8) {
                MC2VR_LOG("view/hmd: SPLIT VP upload start=c%u count=%u (block c%u/%u) — "
                          "passed through", start_register, vec4_count, base, vcd_count);
            }
            return data;
        }
        // camPos row alone: reuse this pass's rebuilt position.
        if (g_pw_valid) {
            math::store3(camrow, g_pw);
            g_hmd_cam_only++;
            return scratch;
        }
        return data;
    }
    const float *raw = data + (base - start_register) * 4;
    vpcam::Camera game, cam;
    if (!vpcam::decompose(raw, &game)) {
        g_hmd_decomp_fail++;
        if (g_hmd_fail_logged++ < 8) {
            MC2VR_LOG("view/hmd: decompose FAILED c%u: |r3.xyz|=%.4f r0.w=%.3f "
                      "r2=[%.3f %.3f %.3f %.3f] — passed through", base,
                      (double)math::length(math::load3(raw + 12)), (double)raw[3],
                      (double)raw[8], (double)raw[9], (double)raw[10], (double)raw[11]);
        }
        return data;
    }
    if (identity) {
        cam = game;
        g_hmd_resid_max = std::fmax(g_hmd_resid_max, vpcam::rebuild_residual(raw, game));
    } else {
        cam = vpcam::apply_eye(game, g_hmd.eye[eye], g_world_scale);
    }
    vpcam::rebuild(cam, scratch + (base - start_register) * 4);
    if (camrow) {
        math::store3(camrow, cam.C);
    }
    g_pw = cam.C;
    g_pw_valid = true;
    g_hmd_blocks++;
    g_rows_rewritten += 4;
    return scratch;
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

    if (g_mode == RewriteMode::Hmd || g_mode == RewriteMode::HmdIdentity) {
        return pass_is_main() ? hmd_rewrite(start_register, data, vec4_count) : data;
    }

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
            // Fires every frame; keeps up with camera rotation within a frame.
            refresh_right_axis(raw);
        }
        if (vp_idx != 0xffffffffu && row[3] != 1.0f) {
            row[3] -= math::dot(math::load3(row), math::load3(d));
            // Asymmetric per-eye projection centre (stereo mode, S2 step 1):
            // row_k.xyz = P_kk * basis_k, so |row_k.xyz| is the projection
            // coefficient that scales the NDC shift into row_k.w. Sign
            // convention to be validated against the HMD runtime (S4).
            if (asym[0] != 0.0f && vp_idx == 0) {
                row[3] += math::length(math::load3(row)) *
                          asym[0];
            } else if (asym[1] != 0.0f && vp_idx == 1) {
                row[3] += math::length(math::load3(row)) *
                          asym[1];
            }
            g_rows_rewritten++;
        } else if (is_pos && row[3] == 1.0f) {
            math::store3(row, math::load3(row) + math::load3(d));
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
                       : g_mode == RewriteMode::Hmd ? "hmd"
                       : g_mode == RewriteMode::HmdIdentity ? "hmd_identity"
                                                       : "off";
    MC2VR_LOG("view: uploads calls=%llu vec4s=%llu | rewritten rows=%llu (mode=%s%s)",
              (unsigned long long)g_vs_calls, (unsigned long long)g_vs_vec4s,
              (unsigned long long)g_rows_rewritten, mode,
              g_mode == RewriteMode::Stereo
                  ? (g_stereo_eye > 0 ? ", eye=R" : ", eye=L")
                  : "");
    if (g_mode == RewriteMode::Hmd || g_mode == RewriteMode::HmdIdentity) {
        MC2VR_LOG("view/hmd: blocks=%llu camOnly=%llu split=%llu decompFail=%llu "
                  "identityResidMax=%.3g poseId=%u valid=%d",
                  (unsigned long long)g_hmd_blocks, (unsigned long long)g_hmd_cam_only,
                  (unsigned long long)g_hmd_split, (unsigned long long)g_hmd_decomp_fail,
                  (double)g_hmd_resid_max, g_hmd.id, (int)g_hmd.valid);
        g_hmd_blocks = g_hmd_cam_only = g_hmd_split = g_hmd_decomp_fail = 0;
        g_hmd_resid_max = 0.0f;
    }
    // Record upload tally (E1 prep): which viewContext record the passes read.
    // The world view's record dominates main-RT uploads; offscreen dominance
    // identifies the shadow-atlas records. Both double-buffer bases of the
    // same idx should show near-identical counts (buffer flip parity).
    for (uint32_t k = 0; k < g_rec_seen_n; k++) {
        if (g_rec_seen[k].main_hits != 0 || g_rec_seen[k].off_hits != 0) {
            MC2VR_LOG("view: rec %p uploads main=%llu offscreen=%llu",
                      (void *)g_rec_seen[k].va,
                      (unsigned long long)g_rec_seen[k].main_hits,
                      (unsigned long long)g_rec_seen[k].off_hits);
            g_rec_seen[k].main_hits = 0;
            g_rec_seen[k].off_hits = 0;
        }
    }
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

void set_view_world_scale(float units_per_metre)
{
    if (units_per_metre > 0.0f && units_per_metre <= 1000.0f) {
        g_world_scale = units_per_metre;
        MC2VR_LOG("view: world scale = %g game units per metre", (double)units_per_metre);
    } else {
        MC2VR_LOG("view: view_world_scale=%g out of range (0,1000], keeping %g",
                  (double)units_per_metre, (double)g_world_scale);
    }
}

uint32_t current_pose_id()
{
    return g_hmd.valid && g_mode == RewriteMode::Hmd ? g_hmd.id : 0;
}

void set_pass_eye(int sign)
{
    // Pass 1 start (-1 = LEFT): sample the HMD pose ONCE for the whole frame —
    // both eyes render with it and the id travels in FRAME_READY.
    if (sign < 0 && g_mode == RewriteMode::Hmd) {
        Mc2IpcState st;
        const bool sane = ipc::read_state(&st) && (st.flags & MC2VR_IPC_STF_TRACKED) &&
                          eye_is_sane(st.eye[0]) && eye_is_sane(st.eye[1]);
        g_hmd.valid = sane;
        if (sane) {
            g_hmd.id = st.hostFrame + 1;
            g_hmd.eye[0] = to_eye_pose(st.eye[0]);
            g_hmd.eye[1] = to_eye_pose(st.eye[1]);
        }
    }
    if (sign != g_pass_eye) {
        g_pw_valid = false;  // per-pass camPos cache
    }
    g_pass_eye = sign;
}

} // namespace mc2vr::view
