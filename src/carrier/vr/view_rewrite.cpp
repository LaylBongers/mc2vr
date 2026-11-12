// Per-eye camera injection — see view_rewrite.hpp and docs/view_rewrite.md.

#include "vr/view_rewrite.hpp"

#include "vr/shader_constants.hpp"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "game/addresses.h"
#include "game/dx9.hpp"
#include "log.hpp"
#include "vr/vec_math.hpp"
#include "vr/camera_table.hpp"
#include "vr/vp_camera.hpp"
#include "vr/xr_session.hpp"

namespace mc2vr::view_rewrite {

namespace {

using math::Vec3;

constexpr uint32_t VS_ROWS = 256;  // vs_3_0 float constant registers (plus headroom)
constexpr uint32_t PS_ROWS = 224;  // ps_3_0 float constant registers

// HmdDelta = I1 (docs/camera.md): the per-eye complement to the
// camtable union injection — the raw VP already carries the union pose, so
// this applies ONLY the per-eye projection (OpenXR FOV) + per-eye position
// delta (eye pose relative to the table's union pose).
// HmdIdentity = the same decompose/rebuild with the game's OWN camera and
// projection — output must equal input (self-check of the decomposition;
// also the clean pass-through channel for diagnostic probe runs). The old
// `hmd` full-VP-replacement mode and the early verification modes (on/pulse/
// stereo row-shift pans) were REMOVED 2026-10-07 — superseded by
// view_table_inject + hmd_delta (live-verified).
enum class RewriteMode { HmdDelta, HmdIdentity };
RewriteMode g_mode = RewriteMode::HmdDelta;

// ---- live technique constant map (published at the upload gate) ------------
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
// E1 prep (docs/camera.md): viewContext RECORD stats from
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

// ---- pass gate (RT0 size) -----------------------------------------------------
// Shadow-map, reflection and other offscreen passes upload their own
// viewContextData; shifting those would move e.g. the shadow map relative to
// its receivers. Gate on RT0 == the engine's LIVE render size (RenderShell
// +0x2b98/+0x2b9c = g_ScreenWidth/Height — every full-size chain surface is
// created from it; docs/reverse_engineering/rt_dims_source.md). Stock it
// equals the backbuffer size; it follows resolution changes (the old gate
// cached the first backbuffer size and silently stopped rewriting after a
// menu resolution change — live 2026-11-08) and render/window decoupling.
uint32_t g_main_w = 0, g_main_h = 0; // last logged render size
uint32_t g_rt_w = 0, g_rt_h = 0;
uint32_t g_rt_seen[16][2];  // distinct RT0 sizes (log once each)
uint32_t g_rt_seen_n = 0;

// ---- per-pass eye state ----------------------------------------------------------
int g_pass_eye = 0;  // Per-pass eye (eye_replay.cpp): -1 pass 1, +1 pass 2, 0 none

// ---- mono camera matrices outside viewContextData ----------------------------
// Shaders without viewContextData get the camera as explicit matrices the game
// computes from the mono camera (docs/view_rewrite.md#explicit-camera-matrices):
//   - clip space, LocalToProj = VP * World, g_ViewProjMtx = VP: D * M with this
//     pass's clip delta D (D * VP_mono = VP_eye);
//   - view space, the projected shadow boxes' LocalToView = View * World (VS)
//     and its inverse ViewToLocal (PS): T * M and M * T^-1 with the view delta
//     T, in the game's x-negated view frame.
uint32_t g_l2p_reg = REG_INVALID;  // the current record's shader (on_record_shader)
uint32_t g_vpm_reg = REG_INVALID;
uint32_t g_l2v_reg = REG_INVALID;
double g_delta[16];  // this pass's clip delta (vp_camera::clip_delta), double
// The game's view space (LocalToView) is vp_camera's with x negated:
// LocalToView_game = S * View_ours * World, S = diag(-1, 1, 1, 1)
// (P_mono * S * LocalToView = LocalToProj on every record; checked per record,
// check_view_convention). T is applied in that frame: S * T * S.
constexpr double kGameViewSigns[4] = {-1.0, 1.0, 1.0, 1.0};
constexpr double kViewConventionTolerance = 5e-3;  // a sign error gives ~1
double g_view_delta[16];  // this pass's T in the game's view space: S * view_delta * S
double g_view_delta_inv[16];  // its inverse (rigid), for ViewToLocal
vp_camera::Camera g_pass_mono;  // the mono camera g_delta / g_view_delta start from
bool g_delta_valid = false;  // for this pass (g_delta, g_view_delta, g_pass_mono)
// The current record's mono LocalToProj / LocalToView, for the checks.
float g_rec_l2p[16], g_rec_l2v[16];
bool g_rec_l2p_seen = false, g_rec_l2v_seen = false;
// ViewToLocal (the projected shadow PS, rows c0-c2: local = ViewToLocal *
// view point) of the current LocalToView record: register from the bound PS's
// constant table, looked up at the record's first stream PS upload.
constexpr uint32_t kV2lRows = 3;
uint32_t g_v2l_reg = REG_INVALID;
bool g_v2l_resolved = false;
double g_v2l_resid_max = 0.0;  // worst |ViewToLocal * LocalToView - I| (mono) this window
uint64_t g_v2l_rows = 0, g_v2l_checks = 0, g_v2l_bad = 0, g_v2l_split = 0, g_v2l_no_reg = 0;
double g_l2v_resid_max = 0.0;  // worst convention residual this window
uint64_t g_l2v_rows = 0, g_l2v_checks = 0, g_l2v_bad = 0, g_l2v_no_l2p = 0;
// This pass's eye camera with the camera at the origin (rotation + per-eye
// projection, depth terms of the main view): the target for camera-relative
// matrices (camera_relative_rewrite).
float g_eye_rel[16];
bool g_eye_rel_valid = false;  // for this pass
uint64_t g_mtx_rows = 0, g_mtx_no_delta = 0, g_mtx_split = 0, g_mtx_rel_rows = 0;

uint32_t render_w()
{
    return *(volatile const uint32_t *)(MC2_G_RENDERSHELL + MC2_RS_SCREEN_W_OFF);
}
uint32_t render_h()
{
    return *(volatile const uint32_t *)(MC2_G_RENDERSHELL + MC2_RS_SCREEN_H_OFF);
}

bool pass_is_main()
{
    // Render size not yet set up (boot): don't gate.
    const uint32_t w = render_w(), h = render_h();
    return w == 0 || (g_rt_w == w && g_rt_h == h);
}

// ---- window counters ------------------------------------------------------------
uint64_t g_vs_calls = 0, g_vs_vec4s = 0, g_rows_rewritten = 0;
uint64_t g_rows_rewritten_total = 0; // monotonic (never reset) — rewrite_count()

} // namespace

// Publish the CURRENT technique's resolved constant map.
void on_upload_gate(uintptr_t tech, uintptr_t rec)
{
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
    // Record discovery (E1 prep, docs/camera.md): rec ([esp+0x18] at the
    // gate) is this pass's viewContext record pointer (game/addresses.h).
    // Each distinct record logs once with the live table base + index; every
    // hit also counts toward the per-window main/offscreen tally so the world
    // view's record identifies by upload dominance (all 24 views are walked
    // uniformly, so walk counts cannot discriminate — 2026-10-06 run 1).
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

namespace {

// ---- HMD camera channel (per-eye complement to the camtable union) ---------
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
    uint32_t id = 0;  // xr_session pose id the frame renders with
    vp_camera::EyePose eye[2];
    xr_session::ViewState views;  // the same views as the session gave them (for the layer)
};
HmdSnapshot g_hmd;

ULONGLONG g_hmd_ms = 0;  // last frame with sane views
float g_world_scale = 1.0f;  // game world units per metre (VERIFIED: metres — gravity 9.81, docs/reverse_engineering/world_units.md)

// Per-pass cache of the rebuilt camera position, for uploads that carry only
// the camPos row (VP rows arrived in an earlier call).
Vec3 g_pw;
bool g_pw_valid = false;

// E2 probe input: the RAW (pre-rewrite) game camera from the latest
// main-pass upload (see get_game_camera in the header).
vp_camera::Camera g_game_cam;
uint64_t g_game_cam_ms = 0;

uint64_t g_hmd_blocks = 0, g_hmd_split = 0, g_hmd_decomp_fail = 0, g_hmd_cam_only = 0;
float g_hmd_resid_max = 0.0f;  // max |rebuilt - raw| over the window (identity self-check)
uint32_t g_hmd_split_logged = 0, g_hmd_fail_logged = 0;

// HMD view stamps (on_hmd_view_built): the last few HMD views' VPs with the
// HMD sample and union pose their camera was rewritten with. Written by the
// builder (scene build), read by the upload path (render thread), seqlocked.
struct Stamp {
    std::atomic<uint32_t> seq{0};
    float vp[16];  // the builder's VP transposed: the upload's layout
    camera_table::UnionSample sample;
    math::Quat rot;
    math::Vec3 pos;
};
constexpr uint32_t kStamps = 8;  // 2 HMD views per frame, a few frames back
Stamp g_stamps[kStamps];
std::atomic<uint32_t> g_stamp_next{0};
uint64_t g_stamp_hits = 0, g_stamp_misses = 0;

bool find_stamp(const float *vp, math::Quat *rot, math::Vec3 *pos, camera_table::UnionSample *sample)
{
    for (Stamp &st : g_stamps) {
        const uint32_t s0 = st.seq.load(std::memory_order_acquire);
        if (s0 == 0 || (s0 & 1u) != 0) continue;
        float v[16];
        memcpy(v, st.vp, sizeof v);
        const math::Quat q = st.rot;
        const math::Vec3 p = st.pos;
        const camera_table::UnionSample smp = st.sample;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (st.seq.load(std::memory_order_relaxed) != s0) continue;
        if (memcmp(v, vp, sizeof v) == 0) {
            *rot = q;
            *pos = p;
            *sample = smp;
            return true;
        }
    }
    return false;
}

// Pose age: the HMD sample the frame was built with (the camera-table union a
// record carries: view, culling, every record) vs the one its eye passes
// render with (this frame's OpenXR views). Measured once per frame at pass 1's
// first matched upload. The rotation between the two is what the per-pass eye
// delta corrects; a design that bakes the eyes in at build time would show it.
bool g_age_measured = false;  // this pass
uint64_t g_age_n = 0, g_age_frames_sum = 0, g_age_frames_max = 0, g_age_unknown = 0;
double g_age_ms_sum = 0.0, g_age_ms_max = 0.0, g_age_deg_sum = 0.0, g_age_deg_max = 0.0;
constexpr int kAgeHist = 4;  // age in OpenXR frames: 0, 1, 2, 3+
uint64_t g_age_hist[kAgeHist] = {};

// The main view camera's pitch (cull_probe's bins): pass 1's first main-view
// upload, world +y up.
std::atomic<float> g_last_pitch{1000.0f};  // main_camera_pitch; 1000 = none yet
bool g_pitch_seen = false;  // this pass

void note_pitch(const vp_camera::Camera &game)
{
    if (g_pass_eye >= 0 || g_pitch_seen) {
        return;
    }
    g_pitch_seen = true;
    const float fy = game.F.y < -1.0f ? -1.0f : game.F.y > 1.0f ? 1.0f : game.F.y;
    g_last_pitch.store(std::asin(fy) * 57.29578f, std::memory_order_relaxed);
}

void measure_pose_age(const math::Quat &u_rot, const camera_table::UnionSample &smp)
{
    if (g_age_measured || g_pass_eye >= 0) {
        return;  // once per frame, pass 1 (LEFT, sign -1)
    }
    g_age_measured = true;
    if (smp.pose_id == 0 || g_hmd.id < smp.pose_id) {
        g_age_unknown++;
        return;
    }
    const uint64_t frames = g_hmd.id - smp.pose_id;
    const double ms = (double)(g_hmd.views.display_time - smp.display_time) * 1e-6;
    // Rotation between the build-time union and this frame's mid-eye pose.
    math::Quat a = g_hmd.eye[0].rot, b = g_hmd.eye[1].rot;
    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0.0f) b = {-b.x, -b.y, -b.z, -b.w};
    math::Quat mid{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
    const float n = std::sqrt(math::norm_sq(mid));
    double deg = 0.0;
    if (n > 1e-6f) {
        mid = {mid.x / n, mid.y / n, mid.z / n, mid.w / n};
        const double d = std::fabs((double)(u_rot.x * mid.x + u_rot.y * mid.y + u_rot.z * mid.z + u_rot.w * mid.w));
        deg = 2.0 * std::acos(d > 1.0 ? 1.0 : d) * 57.29577951;
    }
    g_age_n++;
    g_age_frames_sum += frames;
    if (frames > g_age_frames_max) g_age_frames_max = frames;
    g_age_hist[frames < (uint64_t)kAgeHist ? frames : kAgeHist - 1]++;
    g_age_ms_sum += ms;
    if (ms > g_age_ms_max) g_age_ms_max = ms;
    g_age_deg_sum += deg;
    if (deg > g_age_deg_max) g_age_deg_max = deg;
}

// Published eye (OpenXR view) -> pure-math eye.
vp_camera::EyePose to_eye_pose(const xr_session::EyePose &e)
{
    return {{e.pos.x, e.pos.y, e.pos.z},
            {e.rot.x, e.rot.y, e.rot.z, e.rot.w},
            e.fov.left, e.fov.right, e.fov.up, e.fov.down};
}

// A published eye is usable if its quaternion is unit and its FOV non-degenerate.
bool eye_is_sane(const xr_session::EyePose &e)
{
    const float n = math::norm_sq({e.rot.x, e.rot.y, e.rot.z, e.rot.w});
    return n >= 0.98f && n <= 1.02f && e.fov.right > e.fov.left && e.fov.up > e.fov.down;
}

const float *hmd_rewrite(uint32_t start_register, const float *data, uint32_t vec4_count)
{
    const bool identity = g_mode == RewriteMode::HmdIdentity;
    const int eye = g_pass_eye > 0 ? 1 : 0;
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
    vp_camera::Camera game, cam;
    math::Vec3 eye_off{0.0f, 0.0f, 0.0f};  // cam.C - game.C, from the eye pose itself
    if (!vp_camera::decompose(raw, &game)) {
        g_hmd_decomp_fail++;
        if (g_hmd_fail_logged++ < 8) {
            MC2VR_LOG("view/hmd: decompose FAILED c%u: |r3.xyz|=%.4f r0.w=%.3f "
                      "r2=[%.3f %.3f %.3f %.3f] — passed through", base,
                      (double)math::length(math::load3(raw + 12)), (double)raw[3],
                      (double)raw[8], (double)raw[9], (double)raw[10], (double)raw[11]);
        }
        return data;
    }
    // E2 probe: publish the RAW game camera (pre-rewrite; this site is only
    // reached on main-pass uploads) — BEFORE any pose-validity gate so the
    // consistency oracle (and the camtable probe) works without a tracked
    // HMD too.
    g_game_cam = game;
    g_game_cam_ms = GetTickCount64();
    if (identity) {
        cam = game;
        g_hmd_resid_max = std::fmax(g_hmd_resid_max, vp_camera::rebuild_residual(raw, game));
    } else if (!g_hmd.valid || g_pass_eye == 0) {
        // No pose for this frame/pass — pass the RAW rows through untouched.
        return data;
    } else if (g_mode == RewriteMode::HmdDelta) {
        // I1: the raw VP already carries the camtable UNION pose; apply only
        // the per-eye complement — projection from this eye's OpenXR FOV,
        // position/rotation delta relative to the union pose THIS VP carries
        // (matched by its basis, so the rendered eye equals the pass-1 sample
        // whose id travels with the eye pair even if the VP came from an older
        // table fill), else the same-frame union.
        math::Quat u_rot;
        math::Vec3 u_pos;
        camera_table::UnionSample smp;
        // The HMD view this VP was built for, exactly: the builder's stamp
        // (on_hmd_view_built) carries the union pose the VP was built with
        // and its HMD sample. No stamp = not an HMD view: passed through.
        if (!find_stamp(raw, &u_rot, &u_pos, &smp)) {
            g_stamp_misses++;
            return data;
        }
        g_stamp_hits++;
        measure_pose_age(u_rot, smp);
        note_pitch(game);
        const vp_camera::EyePose &pose = g_hmd.eye[eye];
        vp_camera::EyePose delta;
        // apply_eye maps the delta through the game camera's R/U/F, which
        // already carry the union (head) rotation — so the delta must be
        // expressed in the HEAD frame, not XR local space. Otherwise the eye
        // baseline only lies along the camera's right axis when the head
        // faces its initial yaw (wrong vertical/depth disparity elsewhere,
        // most visible at near range).
        const math::Quat u_inv = math::conj(u_rot);
        delta.pos = math::rotate(u_inv, pose.pos - u_pos);
        delta.rot = u_inv * pose.rot;
        delta.fov_left = pose.fov_left;
        delta.fov_right = pose.fov_right;
        delta.fov_up = pose.fov_up;
        delta.fov_down = pose.fov_down;
        // The decomposed R is the SCREEN-right axis by construction (clip.x =
        // a·dot(R,p−C)+c·z with a>0), so XR +x (eye right) must map to +R: no
        // flip. (An earlier R-negation put each eye on the wrong side:
        // pseudoscopic parallax, invisible at distance but uncomfortable for
        // near objects. Live-verified fixed 2026-10-09.)
        cam = vp_camera::apply_eye(game, delta, g_world_scale);
        eye_off = vp_camera::eye_offset(game, delta, g_world_scale);
    } else {
        return data;  // unreachable: Off/On/Pulse/Stereo never reach hmd_rewrite
    }
    vp_camera::rebuild(cam, scratch + (base - start_register) * 4);
    {
        vp_camera::Camera rel = cam;
        rel.C = {0.0f, 0.0f, 0.0f};
        vp_camera::rebuild(rel, g_eye_rel);
        g_eye_rel_valid = true;
    }
    // D for this pass's explicit camera matrices (matrix_rewrite), from the
    // two cameras' parameters: no inverse of the world-space VP, whose ~1e3
    // translations would round into every corrected matrix.
    vp_camera::clip_delta(game, cam, eye_off, g_delta);
    // T for view-space matrices (LocalToView): the factor inside D.
    vp_camera::view_delta(game, cam, eye_off, g_view_delta);
    math::diag_conjugate4(g_view_delta, kGameViewSigns, g_view_delta);
    math::rigid_inverse4(g_view_delta, g_view_delta_inv);
    g_pass_mono = game;
    g_delta_valid = true;
    if (camrow) {
        math::store3(camrow, cam.C);
    }
    g_pw = cam.C;
    g_pw_valid = true;
    g_hmd_blocks++;
    g_rows_rewritten += 4;
    g_rows_rewritten_total += 4;
    return scratch;
}

} // namespace

bool set_identity_oracle(const char *value)
{
    if (strcmp(value, "on") == 0) {
        g_mode = RewriteMode::HmdIdentity;
    } else if (strcmp(value, "off") == 0) {
        g_mode = RewriteMode::HmdDelta;
    } else {
        return false;
    }
    MC2VR_LOG("view: debug_view_identity=%s (%s)", value,
              g_mode == RewriteMode::HmdIdentity
                  ? "rebuild with the game's own camera — output must equal input"
                  : "per-eye HMD rewrite");
    return true;
}

void on_hmd_view_built(const float *vp, const camera_table::UnionSample &sample, const math::Quat &rot,
                       const math::Vec3 &pos)
{
    Stamp &st = g_stamps[g_stamp_next.fetch_add(1, std::memory_order_relaxed) % kStamps];
    const uint32_t s = st.seq.load(std::memory_order_relaxed);
    st.seq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    // The builder's VP is a D3DX row-vector matrix (p * M, translation in
    // row 3); viewContextData uploads its transpose (clip_i = row_i . p).
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) st.vp[r * 4 + c] = vp[c * 4 + r];
    }
    st.sample = sample;
    st.rot = rot;
    st.pos = pos;
    std::atomic_thread_fence(std::memory_order_release);
    st.seq.store(s + 2, std::memory_order_relaxed);
}

bool main_camera_pitch(float *degrees)
{
    const float p = g_last_pitch.load(std::memory_order_relaxed);
    if (p > 999.0f) return false;
    *degrees = p;
    return true;
}

bool main_pass_active()
{
    return pass_is_main();
}

bool is_main_size(uint32_t w, uint32_t h)
{
    return w == render_w() && h == render_h();
}

void on_set_render_target(uint32_t w, uint32_t h)
{
    g_rt_w = w;
    g_rt_h = h;
    if (render_w() != g_main_w || render_h() != g_main_h) {
        g_main_w = render_w();
        g_main_h = render_h();
        g_rt_seen_n = 0; // re-log the RT0 sizes against the new render size
        MC2VR_LOG("view: main scene RT size = %ux%u (engine render size)", g_main_w, g_main_h);
    }
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
                  pass_is_main()
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
namespace {

// A camera-relative VP (the sky/fog matrices of the clouds pass: rows 0, 1 and
// 3 have w = 0, i.e. the camera sits at the origin of its input space; its depth
// row is its own, z = A'·z_v + B'). D maps the MONO VIEW's clip space: D·m
// reinterprets m's depth with the main view's depth terms, so the result gets
// the main view's slope A (~1.0004 instead of 1.0000019) and distant fog
// lands past the far plane, clipped (docs/view_rewrite.md). Instead build m
// directly: this pass's eye rotation and projection at the origin, m's own
// depth terms. False (m untouched) when m isn't camera-relative or not of the
// assumed form.
bool camera_relative_rewrite(float *m)
{
    constexpr float EPS = 1e-6f;
    if (std::fabs(m[3]) > EPS || std::fabs(m[7]) > EPS || std::fabs(m[15]) > EPS) {
        return false;
    }
    const math::Vec3 r2 = math::load3(m + 8), r3 = math::load3(m + 12);
    const float f2 = math::dot(r3, r3);
    if (std::fabs(f2 - 1.0f) > 1e-3f) {
        return false;  // row3 must be the unit forward axis
    }
    const float a_depth = math::dot(r2, r3);  // A' (|row3| = 1)
    if (math::length(r2 - r3 * a_depth) > 1e-3f * (std::fabs(a_depth) + 1.0f)) {
        return false;  // depth row not along forward
    }
    const float b_depth = m[11];
    memcpy(m, g_eye_rel, sizeof(g_eye_rel));
    const math::Vec3 f_eye = math::load3(g_eye_rel + 12);
    math::store3(m + 8, f_eye * a_depth);
    m[11] = b_depth;
    return true;
}

// The pinned view convention as a check, once per record: P_mono * S *
// LocalToView must reproduce LocalToProj (both mono; LocalToProj is uploaded
// first).
void check_view_convention(const float *l2v)
{
    if (!g_rec_l2p_seen) {
        g_l2v_no_l2p++;
        return;
    }
    double p[16];
    float view_ours[16], clip[16];
    vp_camera::projection(g_pass_mono, p);
    math::scale_rows4(kGameViewSigns, l2v, view_ours);
    math::mul4(p, view_ours, clip);
    const double r = math::max_relative_diff(clip, g_rec_l2p, 16);
    g_l2v_checks++;
    g_l2v_resid_max = std::fmax(g_l2v_resid_max, r);
    if (r > kViewConventionTolerance) {
        g_l2v_bad++;
    }
}

// The current record's explicit camera matrices, corrected by this pass's D
// (clip-space matrices) or T (LocalToView).
const float *matrix_rewrite(uint32_t start_register, const float *data, uint32_t vec4_count)
{
    if ((g_l2p_reg == REG_INVALID && g_vpm_reg == REG_INVALID && g_l2v_reg == REG_INVALID) ||
        vec4_count > VS_ROWS) {
        return data;
    }
    static float scratch[VS_ROWS * 4];
    bool copied = false;
    const uint32_t end = start_register + vec4_count;
    for (const uint32_t reg : {g_l2p_reg, g_vpm_reg, g_l2v_reg}) {
        if (reg == REG_INVALID || !(start_register < reg + 4 && end > reg)) {
            continue;  // not this upload
        }
        if (start_register > reg || end < reg + 4) {
            g_mtx_split++;  // the matrix split across uploads: passed through
            continue;
        }
        if (!g_delta_valid) {
            g_mtx_no_delta++;  // no viewContextData rewrite in this pass yet
            continue;
        }
        if (!copied) {
            memcpy(scratch, data, vec4_count * 16);
            copied = true;
        }
        float *m = scratch + (reg - start_register) * 4;
        float out[16];
        if (reg == g_l2v_reg) {
            memcpy(g_rec_l2v, m, sizeof(g_rec_l2v));  // mono, before its correction
            g_rec_l2v_seen = true;
            check_view_convention(m);
            math::mul4(g_view_delta, m, out);
            memcpy(m, out, sizeof(out));
            g_l2v_rows += 4;
            continue;
        }
        if (reg == g_l2p_reg) {
            memcpy(g_rec_l2p, m, sizeof(g_rec_l2p));  // mono, before its correction
            g_rec_l2p_seen = true;
        }
        if (g_eye_rel_valid && camera_relative_rewrite(m)) {
            g_mtx_rel_rows += 4;
            continue;
        }
        math::mul4(g_delta, m, out);
        memcpy(m, out, sizeof(out));
        g_mtx_rows += 4;
    }
    return copied ? scratch : data;
}

} // namespace

void on_record_shader(IDirect3DVertexShader9 *vs)
{
    g_l2p_reg = g_vpm_reg = g_l2v_reg = g_v2l_reg = REG_INVALID;
    g_rec_l2p_seen = g_rec_l2v_seen = g_v2l_resolved = false;
    const shader_constants::Table *t = shader_constants::of(vs);
    if (t == nullptr) {
        return;
    }
    if (const shader_constants::Named *c = t->find("LocalToProj"); c != nullptr && c->count >= 4) {
        g_l2p_reg = c->reg;
    }
    if (const shader_constants::Named *c = t->find("g_ViewProjMtx"); c != nullptr && c->count >= 4) {
        g_vpm_reg = c->reg;
    }
    if (const shader_constants::Named *c = t->find("LocalToView"); c != nullptr && c->count >= 4) {
        g_l2v_reg = c->reg;
    }
}

namespace {

// The bound pixel shader's ViewToLocal register (the pass pick binds the PS
// before the record's stream runs, so it is this record's).
void resolve_v2l_reg()
{
    g_v2l_resolved = true;
    IDirect3DDevice9 *const dev = game::lti_device();
    IDirect3DPixelShader9 *ps = nullptr;
    if (dev == nullptr || FAILED(dev->GetPixelShader(&ps)) || ps == nullptr) {
        return;
    }
    const shader_constants::Table *t = shader_constants::of(ps);
    ps->Release();  // the cache keys on the pointer; the shader lives on in the device
    if (t == nullptr) {
        return;
    }
    if (const shader_constants::Named *c = t->find("ViewToLocal"); c != nullptr && c->count >= kV2lRows) {
        g_v2l_reg = c->reg;
    }
}

// Mono check: ViewToLocal must invert this record's LocalToView.
void check_v2l(const float *v2l_rows)
{
    if (!g_rec_l2v_seen) {
        return;
    }
    float v2l[16], prod[16];
    double ident[16];
    math::expand_affine4(v2l_rows, v2l);
    math::mul4(v2l, g_rec_l2v, prod);
    math::identity4(ident);
    const double r = math::max_abs_diff(prod, ident, 12);
    g_v2l_checks++;
    g_v2l_resid_max = std::fmax(g_v2l_resid_max, r);
    if (r > 1e-3) {
        g_v2l_bad++;
    }
}

} // namespace

const float *on_record_ps_constant(uint32_t start_register, const float *data, uint32_t vec4_count)
{
    if (g_l2v_reg == REG_INVALID || data == nullptr || vec4_count > PS_ROWS || !pass_is_main()) {
        return data;
    }
    if (!g_v2l_resolved) {
        resolve_v2l_reg();
        if (g_v2l_reg == REG_INVALID) {
            g_v2l_no_reg++;
        }
    }
    const uint32_t reg = g_v2l_reg, end = start_register + vec4_count;
    if (reg == REG_INVALID || !(start_register < reg + kV2lRows && end > reg)) {
        return data;  // not this upload
    }
    if (start_register > reg || end < reg + kV2lRows) {
        g_v2l_split++;
        return data;
    }
    if (!g_delta_valid) {
        g_mtx_no_delta++;
        return data;
    }
    static float scratch[PS_ROWS * 4];
    memcpy(scratch, data, vec4_count * 16);
    float *m = scratch + (reg - start_register) * 4;
    check_v2l(m);
    // ViewToLocal_eye = ViewToLocal_mono * (S T S)^-1: the eye-view point the
    // PS rebuilds (from the corrected LocalToView) maps into the same box.
    float v2l[16], v2l_eye[16];
    double out[16];
    math::expand_affine4(m, v2l);
    math::mul4(v2l, g_view_delta_inv, out);
    math::to_float4(out, v2l_eye);
    memcpy(m, v2l_eye, kV2lRows * 16);
    g_v2l_rows += kV2lRows;
    return scratch;
}

const float *on_set_vs_constant(uint32_t start_register, const float *data,
                                uint32_t vec4_count)
{
    g_vs_calls++;
    if (!data || vec4_count == 0) {
        return data;
    }
    g_vs_vec4s += vec4_count;
    if (!pass_is_main()) {
        return data;
    }
    const float *out = hmd_rewrite(start_register, data, vec4_count);
    return out != data ? out : matrix_rewrite(start_register, data, vec4_count);
}

uint64_t rewrite_count()
{
    return g_rows_rewritten_total;
}

void report_window()
{
    const char *mode = g_mode == RewriteMode::HmdDelta ? "hmd_delta" : "hmd_identity";
    MC2VR_LOG("view: uploads calls=%llu vec4s=%llu | rewritten rows=%llu (mode=%s)",
              (unsigned long long)g_vs_calls, (unsigned long long)g_vs_vec4s,
              (unsigned long long)g_rows_rewritten, mode);
    // The rendered game projection, decomposed from the latest main-pass
    // upload (docs/culling.md): centered projection a = 1/tan(halfH),
    // b = 1/tan(halfV) — ADS/zoom fov changes show up here directly.
    if (g_game_cam_ms != 0 && g_game_cam.a > 0.0f && g_game_cam.b > 0.0f) {
        MC2VR_LOG("view: gameproj: a=%.4f b=%.4f -> halfH=%.2fdeg halfV=%.2fdeg",
                  (double)g_game_cam.a, (double)g_game_cam.b,
                  std::atan(1.0 / (double)g_game_cam.a) * 180.0 / 3.14159265,
                  std::atan(1.0 / (double)g_game_cam.b) * 180.0 / 3.14159265);
    }
    {
        MC2VR_LOG("view/hmd: blocks=%llu camOnly=%llu split=%llu decompFail=%llu "
                  "identityResidMax=%.3g poseId=%u valid=%d",
                  (unsigned long long)g_hmd_blocks, (unsigned long long)g_hmd_cam_only,
                  (unsigned long long)g_hmd_split, (unsigned long long)g_hmd_decomp_fail,
                  (double)g_hmd_resid_max, g_hmd.id, (int)g_hmd.valid);
        MC2VR_LOG("view/hmd: camera matrices (LocalToProj, g_ViewProjMtx) rewritten rows=%llu "
                  "cameraRelative=%llu split=%llu noDelta=%llu",
                  (unsigned long long)g_mtx_rows, (unsigned long long)g_mtx_rel_rows,
                  (unsigned long long)g_mtx_split, (unsigned long long)g_mtx_no_delta);
        g_mtx_rows = g_mtx_split = g_mtx_no_delta = g_mtx_rel_rows = 0;
        MC2VR_LOG("view/hmd: shadow boxes: LocalToView rows=%llu ViewToLocal rows=%llu | checks: "
                  "convention (P_mono*S*LocalToView = LocalToProj, S = -x) n=%llu bad=%llu max=%.3g, "
                  "inverse (ViewToLocal*LocalToView = I) n=%llu bad=%llu max=%.3g | noL2P=%llu split=%llu noReg=%llu",
                  (unsigned long long)g_l2v_rows, (unsigned long long)g_v2l_rows,
                  (unsigned long long)g_l2v_checks, (unsigned long long)g_l2v_bad, g_l2v_resid_max,
                  (unsigned long long)g_v2l_checks, (unsigned long long)g_v2l_bad, g_v2l_resid_max,
                  (unsigned long long)g_l2v_no_l2p, (unsigned long long)g_v2l_split,
                  (unsigned long long)g_v2l_no_reg);
        g_l2v_rows = g_l2v_checks = g_l2v_bad = g_l2v_no_l2p = 0;
        g_v2l_rows = g_v2l_checks = g_v2l_bad = g_v2l_split = g_v2l_no_reg = 0;
        g_l2v_resid_max = g_v2l_resid_max = 0.0;
        MC2VR_LOG("view/hmd: HMD views hit=%llu notHmd=%llu (main-pass camera uploads with / without an HMD "
                  "view stamp; notHmd ones pass through)",
                  (unsigned long long)g_stamp_hits, (unsigned long long)g_stamp_misses);
        g_stamp_hits = g_stamp_misses = 0;
        if (g_age_n != 0 || g_age_unknown != 0) {
            const double n = g_age_n ? (double)g_age_n : 1.0;
            MC2VR_LOG("view/hmd: poseAge frames=%llu | OpenXR frames avg %.2f max %llu [0:%llu 1:%llu 2:%llu 3+:%llu] | "
                      "display time avg %.1f max %.1f ms | head rotation avg %.2f max %.2f deg | unknown=%llu",
                      (unsigned long long)g_age_n, (double)g_age_frames_sum / n,
                      (unsigned long long)g_age_frames_max, (unsigned long long)g_age_hist[0],
                      (unsigned long long)g_age_hist[1], (unsigned long long)g_age_hist[2],
                      (unsigned long long)g_age_hist[3], g_age_ms_sum / n, g_age_ms_max, g_age_deg_sum / n,
                      g_age_deg_max, (unsigned long long)g_age_unknown);
        }
        g_age_n = g_age_frames_sum = g_age_frames_max = g_age_unknown = 0;
        g_age_ms_sum = g_age_ms_max = g_age_deg_sum = g_age_deg_max = 0.0;
        for (uint64_t &h : g_age_hist) h = 0;
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

float world_scale()
{
    return g_world_scale;
}

uint32_t current_pose_id()
{
    return g_hmd.valid && g_mode == RewriteMode::HmdDelta ? g_hmd.id : 0;
}

bool get_game_camera(vp_camera::Camera *out)
{
    if (g_game_cam_ms == 0 || GetTickCount64() - g_game_cam_ms > 1000) {
        return false;
    }
    *out = g_game_cam;
    return true;
}

bool mono_clip_to_eye(const double in[4], double out[4])
{
    if (!g_delta_valid) {
        return false;
    }
    math::transform4(g_delta, in, out);
    return true;
}

bool frame_eyes(vp_camera::EyePose out[2])
{
    if (current_pose_id() == 0) {
        return false;
    }
    out[0] = g_hmd.eye[0];
    out[1] = g_hmd.eye[1];
    return true;
}

const xr_session::ViewState *begin_frame(const xr_session::ViewState *views)
{
    if (g_mode != RewriteMode::HmdDelta) {
        return nullptr;  // debug camera modes render without the HMD pose
    }
    const bool sane = views != nullptr && views->tracked && eye_is_sane(views->eye[0]) &&
                      eye_is_sane(views->eye[1]);
    // Without sane views keep the previous ones for a short window (same hold
    // as camtable's sample_pose) so the frame still renders with a head pose.
    const ULONGLONG now = GetTickCount64();
    g_hmd.valid = sane || (g_hmd.valid && now - g_hmd_ms < 250);
    if (sane) {
        g_hmd_ms = now;
        g_hmd.id = views->pose_id;
        g_hmd.eye[0] = to_eye_pose(views->eye[0]);
        g_hmd.eye[1] = to_eye_pose(views->eye[1]);
        g_hmd.views = *views;
    }
    return g_hmd.valid ? &g_hmd.views : nullptr;
}

void set_pass_eye(int sign)
{
    if (sign != g_pass_eye) {
        g_pitch_seen = false;
        g_pw_valid = false;     // per-pass camPos cache
        g_delta_valid = false;  // per-pass camera delta
        g_age_measured = false;
        g_eye_rel_valid = false;
    }
    g_pass_eye = sign;
}

} // namespace mc2vr::view_rewrite
