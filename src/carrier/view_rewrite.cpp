// Per-eye camera injection — see view_rewrite.hpp and docs/stereo_design.md §S2.

#include "view_rewrite.hpp"

#include <safetyhook.hpp>

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "game_addresses.h"
#include "ipc.hpp"
#include "log.hpp"
#include "vec_math.hpp"
#include "camera_table.hpp"
#include "vp_camera.hpp"

namespace mc2vr::view_rewrite {

namespace {

using math::Vec3;

constexpr uint32_t VS_ROWS = 256;  // vs_3_0 float constant registers (plus headroom)

// HmdDelta = I1 (stereo_improvements_plan.md): the per-eye complement to the
// camtable union injection — the raw VP already carries the union pose, so
// this applies ONLY the per-eye projection (OpenXR FOV) + per-eye position
// delta (eye pose relative to the table's union pose).
// HmdIdentity = the same decompose/rebuild with the game's OWN camera and
// projection — output must equal input (self-check of the decomposition;
// also the clean pass-through channel for diagnostic probe runs). The old
// `hmd` full-VP-replacement mode and the S2 verification modes (on/pulse/
// stereo row-shift pans) were REMOVED 2026-10-07 — superseded by
// view_table_inject + hmd_delta (live-verified).
enum class RewriteMode { Off, HmdDelta, HmdIdentity };
RewriteMode g_mode = RewriteMode::Off;

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

// ---- per-pass eye state ----------------------------------------------------------
int g_pass_eye = 0;  // S2c-2 per-pass eye (eye_replay.cpp): -1 pass 1, +1 pass 2, 0 none

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
    if (strcmp(value, "hmd_delta") == 0) {
        return RewriteMode::HmdDelta;
    }
    if (strcmp(value, "hmd_identity") == 0) {
        return RewriteMode::HmdIdentity;
    }
    *ok = false;
    return RewriteMode::Off;
}

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
    uint32_t id = 0;  // pose id (hostFrame+1) the carrier tags frames with
    vp_camera::EyePose eye[2];
};
HmdSnapshot g_hmd;
ULONGLONG g_hmd_ms = 0;  // last successful pass-1 pose sample
float g_world_scale = 1.0f;  // game world units per metre (VERIFIED: metres — gravity 9.81, docs/reverse_engineering/pandemic_engine.md)

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
uint64_t g_delta_no_union = 0;  // hmd_delta uploads skipped: no table union this frame
uint32_t g_delta_warn_logged = 0;

// Host-published eye (IPC layout) -> pure-math eye.
vp_camera::EyePose to_eye_pose(const Mc2IpcEyePose &e)
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
        // position/rotation delta relative to the union pose camtable used
        // (same-frame reference, so pose and delta always pair up).
        math::Quat u_rot;
        math::Vec3 u_pos;
        if (!camera_table::get_union(&u_rot, &u_pos)) {
            g_delta_no_union++;
            if (g_delta_warn_logged++ < 8) {
                MC2VR_LOG("view/hmd: delta upload passed through — no camtable union "
                          "this frame (view_table_inject=on and HMD tracked?)");
            }
            return data;
        }
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
    } else {
        return data;  // unreachable: Off/On/Pulse/Stereo never reach hmd_rewrite
    }
    vp_camera::rebuild(cam, scratch + (base - start_register) * 4);
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

    if (g_mode == RewriteMode::HmdDelta || g_mode == RewriteMode::HmdIdentity) {
        return pass_is_main() ? hmd_rewrite(start_register, data, vec4_count) : data;
    }

    return data;  // no rewrite for this mode — pass the upload through
}

void report_window()
{
    const char *mode = g_mode == RewriteMode::HmdDelta     ? "hmd_delta"
                       : g_mode == RewriteMode::HmdIdentity ? "hmd_identity"
                                                            : "off";
    MC2VR_LOG("view: uploads calls=%llu vec4s=%llu | rewritten rows=%llu (mode=%s)",
              (unsigned long long)g_vs_calls, (unsigned long long)g_vs_vec4s,
              (unsigned long long)g_rows_rewritten, mode);
    // The rendered game projection, decomposed from the latest main-pass
    // upload (frustum_cull_plan.md): centered projection a = 1/tan(halfH),
    // b = 1/tan(halfV) — ADS/zoom fov changes show up here directly.
    if (g_game_cam_ms != 0 && g_game_cam.a > 0.0f && g_game_cam.b > 0.0f) {
        MC2VR_LOG("view: gameproj: a=%.4f b=%.4f -> halfH=%.2fdeg halfV=%.2fdeg",
                  (double)g_game_cam.a, (double)g_game_cam.b,
                  std::atan(1.0 / (double)g_game_cam.a) * 180.0 / 3.14159265,
                  std::atan(1.0 / (double)g_game_cam.b) * 180.0 / 3.14159265);
    }
    if (g_mode == RewriteMode::HmdDelta || g_mode == RewriteMode::HmdIdentity) {
        if (g_mode == RewriteMode::HmdDelta && g_delta_no_union > 0) {
            MC2VR_LOG("view/hmd: deltaNoUnion=%llu (uploads passed through — the "
                      "camtable union was missing for that frame)",
                      (unsigned long long)g_delta_no_union);
            g_delta_no_union = 0;
        }
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

void set_pass_eye(int sign)
{
    // Pass 1 start (-1 = LEFT): sample the HMD pose ONCE for the whole frame —
    // both eyes render with it and the id travels in FRAME_READY.
    if (sign < 0 && g_mode == RewriteMode::HmdDelta) {
        Mc2IpcState st;
        const bool sane = ipc::read_state(&st) && (st.flags & MC2VR_IPC_STF_TRACKED) &&
                          eye_is_sane(st.eye[0]) && eye_is_sane(st.eye[1]);
        // On a miss keep the previous snapshot for a short window (same hold as
        // camtable's sample_pose) so the frame still renders with a head pose.
        const ULONGLONG now = GetTickCount64();
        g_hmd.valid = sane || (g_hmd.valid && now - g_hmd_ms < 250);
        if (sane) {
            g_hmd_ms = now;
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

} // namespace mc2vr::view_rewrite
