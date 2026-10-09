// Union HMD camera injection at g_CameraTable — see camera_table.hpp for the
// design and docs/stereo_improvements_plan.md for the evidence.

#include "camera_table.hpp"

#include <safetyhook.hpp>

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "game_addresses.h"
#include "hooks.hpp"
#include "ipc.hpp"
#include "log.hpp"
#include "vec_math.hpp"
#include "view_rewrite.hpp"
#include "vp_camera.hpp"

namespace mc2vr::camera_table {

using math::Quat;
using math::Vec3;

namespace {

bool g_enabled = false;
SafetyHookMid g_mid;

// ---- pose (sampled once per game frame) ----------------------------------
bool g_pose_valid = false;
uint64_t g_pose_frame = ~0ull;  // frame the pose was last sampled in
uint32_t g_pose_id = 0;
Quat g_pose_rot;
Vec3 g_pose_pos;
uint64_t g_pose_ms = 0;  // last successful pose/fov sample (staleness window)
constexpr uint64_t kPoseHoldMs = 250;  // reuse the last good pose this long on a miss
// Why a sample failed (window counters): seqlock read gave up / host reports
// untracked / eye pose or FOV failed the sanity check; held = a miss bridged
// by the last good pose.
uint64_t g_miss_read = 0, g_miss_untracked = 0, g_miss_insane = 0, g_pose_held = 0;
// HMD FOV union (radians, outermost bound over both eyes) — the cull frustum
// input (cull_frustum.cpp via get_fov_union).
float g_fov_half_h = 0.0f, g_fov_half_v = 0.0f;

// ---- rewritten-entry history (union-camera identification) ---------------
// The view-context builder (cull_frustum.cpp) works on camera entries that are
// copies of the g_CameraTable entries rewritten here. A builder entry whose
// rotation rows + position equal a recent rewrite IS an HMD-driven view;
// reflection/shadow/aux cameras never match. Single writer (the fill loop);
// readers may be on another thread, so each slot carries a seqlock counter
// (odd while being written).
constexpr uint32_t HISTORY = 8;
struct Rewritten {
    std::atomic<uint32_t> seq{0};
    uint64_t frame = 0;
    float rows[9] = {};  // entry+0x10 rows (3 floats each, w skipped)
    float pos[3] = {};
};
Rewritten g_history[HISTORY];
uint32_t g_history_next = 0;

void remember_rewrite(const float *e)
{
    Rewritten &h = g_history[g_history_next];
    g_history_next = (g_history_next + 1) % HISTORY;
    const uint32_t s = h.seq.load(std::memory_order_relaxed);
    h.seq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    h.frame = hooks::frame_count();
    for (int i = 0; i < 3; i++) {
        h.rows[i * 3 + 0] = e[i * 4 + 0];
        h.rows[i * 3 + 1] = e[i * 4 + 1];
        h.rows[i * 3 + 2] = e[i * 4 + 2];
        h.pos[i] = e[12 + i];
    }
    std::atomic_thread_fence(std::memory_order_release);
    h.seq.store(s + 2, std::memory_order_relaxed);
}

uint64_t g_fills = 0, g_rewrites = 0;
uint64_t g_no_pose = 0, g_bad_entry = 0, g_bad_rows = 0;

// Distinct filled entries (EAX values). 5 slots x a handful of self-indexed
// sub-entries each — 8 covers it with margin; overflow only stops the census.
constexpr uint32_t ENTRIES_MAX = 8;
struct EntryStat {
    uintptr_t rot;  // EAX at the hook = entry+0x10
    uint32_t slot;  // slot index (ESI), 0xffffffff when ESI looked wrong
    uint32_t sub;   // self-indexed sub-entry within the slot
    uint64_t fills, rewrites;
};
EntryStat g_entries[ENTRIES_MAX];
uint32_t g_entries_n = 0;

// Sanity mirrors of view_rewrite.cpp's file-local eye_is_sane: the published
// eye must be a unit quaternion with a non-degenerate FOV.
bool eye_is_sane(const Mc2IpcEyePose &e)
{
    const float n = math::norm_sq(Quat{e.rot.x, e.rot.y, e.rot.z, e.rot.w});
    return n >= 0.98f && n <= 1.02f && e.fov.right > e.fov.left && e.fov.up > e.fov.down;
}

// Read the host pose and cache the UNION (mid-eye) pose for this frame. Same
// lock-free seqlock read the S4-4 pass-1 sample in view_rewrite.cpp uses; the
// fill loop runs on the main thread, same as every other IPC consumer.
void sample_pose()
{
    const uint64_t frame = hooks::frame_count();
    if (frame == g_pose_frame) {
        return; // once per frame — all fills in a frame share one pose
    }
    g_pose_frame = frame;
    g_pose_valid = false;

    Mc2IpcState st;
    bool ok = true;
    if (!ipc::read_state(&st)) {
        g_miss_read++;
        ok = false;
    } else if (!(st.flags & MC2VR_IPC_STF_TRACKED)) {
        g_miss_untracked++;
        ok = false;
    } else if (!eye_is_sane(st.eye[0]) || !eye_is_sane(st.eye[1])) {
        g_miss_insane++;
        ok = false;
    }
    if (!ok) {
        // Hold the last good pose for a short window rather than dropping the
        // whole frame to the game's own (head-less, mono) camera: a frame
        // without the union renders AND culls from the wrong view.
        if (g_pose_ms != 0 && GetTickCount64() - g_pose_ms < kPoseHoldMs) {
            g_pose_valid = true;
            g_pose_held++;
        }
        return;
    }

    // Mid rotation: shortest-path quaternion average of the two eyes.
    Quat a{st.eye[0].rot.x, st.eye[0].rot.y, st.eye[0].rot.z, st.eye[0].rot.w};
    Quat b{st.eye[1].rot.x, st.eye[1].rot.y, st.eye[1].rot.z, st.eye[1].rot.w};
    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0.0f) {
        b = {-b.x, -b.y, -b.z, -b.w};
    }
    Quat q{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
    const float n = std::sqrt(math::norm_sq(q));
    if (n < 1e-6f) {
        return; // degenerate average — treat as untracked
    }
    g_pose_rot = {q.x / n, q.y / n, q.z / n, q.w / n};
    g_pose_pos = {0.5f * (st.eye[0].pos.x + st.eye[1].pos.x),
                  0.5f * (st.eye[0].pos.y + st.eye[1].pos.y),
                  0.5f * (st.eye[0].pos.z + st.eye[1].pos.z)};
    g_pose_id = st.hostFrame + 1;
    g_pose_valid = true;
    g_pose_ms = GetTickCount64();

    // Cull frustum input: the FOV UNION over both eyes — half-angles in
    // radians (the IPC fov fields are angles; vp_camera tans them directly).
    // Union = outermost bound on each side: max(|left|,|right|) horizontally,
    // max(|up|,|down|) vertically, across BOTH eyes. Sanity-bounded so a
    // garbage host value never stretches the culling volume.
    const auto &fa = st.eye[0].fov, &fb = st.eye[1].fov;
    const float hh = std::fmax(std::fmax(-fa.left, fa.right),
                               std::fmax(-fb.left, fb.right));
    const float hv = std::fmax(std::fmax(-fa.down, fa.up),
                               std::fmax(-fb.down, fb.up));
    if (hh > 0.05f && hh < 1.5f && hv > 0.05f && hv < 1.5f) {
        g_fov_half_h = hh;
        g_fov_half_v = hv;
    }
}

// ---- camera frame calibration -------------------------------------------
// Runtime verification of the entry convention (probe-derived final model:
// the rendered camera's axes are the entry's ROWS — see the composition
// block in fill_midhook and the plate on CameraTable_FillFromPose). Measures
// sign(dot(row_j, rendered_axis_j)) against the decomposed VP camera
// (view_rewrite::get_game_camera — the physically-validated basis the record path
// renders with); two consecutive agreeing fills lock the signs. While the
// head roughly faces where the game camera points, the dots are ~+-1. The
// rewrite WAITS (counted as frameWaits) until the signs lock — a changed
// measurement between builds means the convention changed: re-run the
// debug_camtable_probe before trusting the union pose.
int8_t g_s0 = 0, g_s1 = 0, g_s2 = 0;  // measured ROW signs; 0 = unlocked
int8_t g_cand0 = 0, g_cand1 = 0, g_cand2 = 0;
uint32_t g_cand_agree = 0;
uint64_t g_frame_waits = 0;
uint64_t g_rewrite_frame = ~0ull;  // frame of the last actual rewrite

// ---- transfer-function probe (debug_camtable_probe) --------------------
// Injects a fixed +10° rotation about one entry-local axis (cycling x/y/z,
// 2s each) and logs the pre/post entry plus the previous frame's rendered
// (decomposed) camera — the instrument that settled the entry convention.
// Diagnostic only; overrides the union rewrite while armed.
bool g_probe = false;
uint64_t g_probe_last_log = 0;

void try_measure_frame(const Vec3 &r0, const Vec3 &r1, const Vec3 &r2);  // below

void probe_step(float *e)
{
    // Column extraction on purpose: the probe documents the transfer for a
    // column-composition E' = E*M(q) — the measured law (rendered = M^-1
    // world-side) came from exactly this form.
    const Vec3 c0 = {e[0], e[4], e[8]};
    const Vec3 c1 = {e[1], e[5], e[9]};
    const Vec3 c2 = {e[2], e[6], e[10]};
    const Vec3 r0 = {e[0], e[1], e[2]};
    const Vec3 r1 = {e[4], e[5], e[6]};
    const Vec3 r2 = {e[8], e[9], e[10]};
    try_measure_frame(r0, r1, r2);  // relative-sign facts go to the log too
    const uint64_t now = GetTickCount64();
    const uint32_t phase = (uint32_t)((now / 2000) % 3);  // x, y, z
    const float half = 10.0f * 3.14159265f / 180.0f * 0.5f;
    const float s = std::sin(half), c = std::cos(half);
    const Quat q = phase == 0 ? Quat{s, 0, 0, c}
                  : phase == 1 ? Quat{0, s, 0, c}
                               : Quat{0, 0, s, c};
    // Raw camera-LOCAL injection, no frame mapping on purpose: E' = E*M(q).
    auto to_world = [&](Vec3 v) { return c0 * v.x + c1 * v.y + c2 * v.z; };
    const Vec3 n0 = to_world(math::rotate(q, {1, 0, 0}));
    const Vec3 n1 = to_world(math::rotate(q, {0, 1, 0}));
    const Vec3 n2 = to_world(math::rotate(q, {0, 0, 1}));
    if (now - g_probe_last_log >= 200) {
        g_probe_last_log = now;
        vp_camera::Camera cam;
        char dec[192];
        int m = 0;
        if (view_rewrite::get_game_camera(&cam)) {
            m = _snprintf(dec, sizeof(dec), "R=(%.3f %.3f %.3f) U=(%.3f %.3f %.3f) "
                         "F=(%.3f %.3f %.3f) C=(%.1f %.1f %.1f)",
                         (double)cam.R.x, (double)cam.R.y, (double)cam.R.z,
                         (double)cam.U.x, (double)cam.U.y, (double)cam.U.z,
                         (double)cam.F.x, (double)cam.F.y, (double)cam.F.z,
                         (double)cam.C.x, (double)cam.C.y, (double)cam.C.z);
        } else {
            m = _snprintf(dec, sizeof(dec), "none-yet");
        }
        dec[m > 0 ? (m < (int)sizeof(dec) - 1 ? m : (int)sizeof(dec) - 1) : 0] = 0;
        MC2VR_LOG("camprobe: axis=%c q=(%.3f %.3f %.3f %.3f) "
                  "E_pre=[%.2f %.2f %.2f|%.2f %.2f %.2f|%.2f %.2f %.2f] "
                  "E_post=[%.2f %.2f %.2f|%.2f %.2f %.2f|%.2f %.2f %.2f] decomp=%s",
                  'x' + (int)phase, (double)q.x, (double)q.y, (double)q.z, (double)q.w,
                  (double)c0.x, (double)c0.y, (double)c0.z,
                  (double)c1.x, (double)c1.y, (double)c1.z,
                  (double)c2.x, (double)c2.y, (double)c2.z,
                  (double)n0.x, (double)n0.y, (double)n0.z,
                  (double)n1.x, (double)n1.y, (double)n1.z,
                  (double)n2.x, (double)n2.y, (double)n2.z, dec);
    }
    e[0] = n0.x; e[4] = n0.y; e[8] = n0.z;
    e[1] = n1.x; e[5] = n1.y; e[9] = n1.z;
    e[2] = n2.x; e[6] = n2.y; e[10] = n2.z;
    g_rewrites++;  // reuse the counters so the window report stays honest
    g_rewrite_frame = hooks::frame_count();
}

void try_measure_frame(const Vec3 &r0, const Vec3 &r1, const Vec3 &r2)
{
    vp_camera::Camera cam;
    if (!view_rewrite::get_game_camera(&cam)) {
        return;  // no decomposed upload yet — keep waiting
    }
    // ROW signs: the rendered basis is s_j * row_j (probe-proven) — measure
    // sign(dot(row_j, rendered_axis_j)) against the decomposed camera.
    const float d0 = math::dot(r0, cam.R);
    const float d1 = math::dot(r1, cam.U);
    const float d2 = math::dot(r2, cam.F);
    if (std::fabs(d0) < 0.85f || std::fabs(d1) < 0.85f || std::fabs(d2) < 0.85f) {
        g_cand_agree = 0;  // head vs camera in relative motion — retry later
        return;
    }
    const int8_t s0 = d0 > 0 ? 1 : -1, s1 = d1 > 0 ? 1 : -1, s2 = d2 > 0 ? 1 : -1;
    if (s0 == g_cand0 && s1 == g_cand1 && s2 == g_cand2 && g_cand_agree > 0) {
        g_cand_agree++;
    } else {
        g_cand0 = s0; g_cand1 = s1; g_cand2 = s2;
        g_cand_agree = 1;
    }
    if (g_cand_agree >= 2) {
        g_s0 = s0; g_s1 = s1; g_s2 = s2;
        MC2VR_LOG("camtable: camera frame MEASURED: row0.R=%+.2f row1.U=%+.2f "
                  "row2.F=%+.2f -> rendered = (%srow0, %srow1, %srow2) — "
                  "union rewrite ACTIVE",
                  (double)d0, (double)d1, (double)d2,
                  s0 > 0 ? "+" : "-", s1 > 0 ? "+" : "-", s2 > 0 ? "+" : "-");
    }
}

void fill_midhook(safetyhook::Context &ctx)
{
    if (!g_enabled && !g_probe) {
        return;  // neither the union rewrite nor the probe is active
    }
    g_fills++;

    // EAX = Matrix_Copy3x4 destination = the just-filled entry+0x10. The fill
    // loop is the function's only caller, so EAX is always inside g_CameraTable
    // — the range check is belt-and-braces against layout surprises.
    const uintptr_t rot = ctx.eax;
    if (rot - MC2_G_CAMTABLE >= MC2_CAMTABLE_SLOT_STRIDE * MC2_CAMTABLE_SLOTS) {
        g_bad_entry++;
        return;
    }

    // ESI = the 0x620 slot base (live across the hooked call — the fill itself
    // uses it after us). slot/sub are census labels, not injection inputs.
    const uintptr_t slot = ctx.esi;
    uint32_t slot_idx = 0xffffffffu, sub_idx = 0xffffffffu;
    if (slot - MC2_G_CAMTABLE < MC2_CAMTABLE_SLOT_STRIDE * MC2_CAMTABLE_SLOTS) {
        slot_idx = (uint32_t)((slot - MC2_G_CAMTABLE) / MC2_CAMTABLE_SLOT_STRIDE);
        const uintptr_t off = (rot - MC2_G_CAMTABLE) % MC2_CAMTABLE_SLOT_STRIDE;
        if (off >= MC2_VCCAM_ENTRY_ROT_OFF) {
            sub_idx = (uint32_t)((off - MC2_VCCAM_ENTRY_ROT_OFF) / MC2_VCCAM_ENTRY_STRIDE);
        }
    }

    EntryStat *st = nullptr;
    for (uint32_t k = 0; k < g_entries_n; k++) {
        if (g_entries[k].rot == rot) {
            st = &g_entries[k];
            break;
        }
    }
    if (!st && g_entries_n < ENTRIES_MAX) {
        st = &g_entries[g_entries_n++];
        *st = {rot, slot_idx, sub_idx, 0, 0};
        MC2VR_LOG("camtable: entry first fill: rot=%p (slot=%u.%u) — union rewrite armed",
                  (void *)rot, slot_idx, sub_idx);
    }
    if (st) {
        st->fills++;
    }

    // entry+0x10: the 3x3 rotation E, ROW-MAJOR, plus position at +0x30
    // (entry+0x40). PROBE-PROVEN CONVENTION (camprobe transfer run
    // 2026-10-07; see the plate on CameraTable_FillFromPose — do not
    // re-derive from static sign analysis):
    //   - the RENDERED camera's axes are the entry's ROWS —
    //     R = s0*r0, U = s1*r1, F = s2*r2 (s = measured row signs;
    //     this game: (-1,+1,+1));
    //   - transfer law: writing E' = E·M renders axes' = M⁻¹·axes — the
    //     builder's 4x4 inverse sits between the entry and the view, so a
    //     naive compose lands inverted and world-side.
    float *e = (float *)rot;
    const Vec3 r0 = {e[0], e[1], e[2]};
    const Vec3 r1 = {e[4], e[5], e[6]};
    const Vec3 r2 = {e[8], e[9], e[10]};
    const Vec3 cpos = math::load3(e + 12);
    auto unit = [](Vec3 v) {
        const float n = math::length(v);
        return n >= 0.5f && n <= 2.0f;
    };
    if (!unit(r0) || !unit(r1) || !unit(r2)) {
        // Not an orthonormal rotation — a layout surprise, never a half-pose.
        g_bad_rows++;
        return;
    }

    // Probe mode: measures the game's transfer function; replaces the union
    // rewrite entirely while armed.
    if (g_probe) {
        probe_step(e);
        return;
    }

    // Frame calibration gate: no rewrite until the row signs are measured.
    // (While unlocked the table stays pure game pose, so the decomposed
    // camera measures against exactly these rows — it self-bootstraps.)
    if (g_s0 == 0) {
        try_measure_frame(r0, r1, r2);
        g_frame_waits++;
        return;
    }

    sample_pose();
    if (!g_pose_valid) {
        g_no_pose++;
        return;
    }

    // DESIRED: rotate the rendered camera by the head rotation L, applied
    // about the camera's OWN axes (aim-following: nod always pitches, lean
    // always rolls — no coupling however the aim is turned). The rendered
    // local frame is (x=right, y=up, z=FORWARD — LH); XR LOCAL is z-back:
    //   L = F_z-conj(q) = (-qx, -qy, +qz, qw)
    // From the probe law (writing E·M renders M⁻¹ world-side), the required
    // write is the closed form  E' = S_r · L⁻¹ · S_r · E  (a LEFT multiply),
    // whose composite quaternion is qB_j = det(S)·s_j·conj(L)_j:
    //   qB = (qx, -qy, +qz, qw)   [s = (-1,+1,+1)]
    // verified against the probe's numbers end-to-end. Position: the head
    // offset maps to world via the rendered axes = s_j·r_j with the XR z
    // flip:  ΔC = (s0*px*r0 + s1*py*r1 + s2*(-pz)*r2) * view_world_scale.
    const float s0 = (float)g_s0, s1 = (float)g_s1, s2 = (float)g_s2;
    const float det = s0 * s1 * s2;
    const Quat &q = g_pose_rot;
    const Quat qB{det * s0 * q.x, det * s1 * q.y, det * s2 * -q.z, q.w};
    // B = rotation matrix of qB (columns b_j = rotate(qB, e_j)); E' = B·E
    // row-wise: row_i(B·E) = Σ_k B[i][k]·r_k, and B[i][k] = b_k[i] — the
    // i-th component of EACH column (NOT all components of one column:
    // that transposes B = applies B⁻¹ — run 7's clean all-axes reversal).
    const Vec3 b0 = math::rotate(qB, {1, 0, 0});
    const Vec3 b1 = math::rotate(qB, {0, 1, 0});
    const Vec3 b2 = math::rotate(qB, {0, 0, 1});
    const Vec3 n0 = b0.x * r0 + b1.x * r1 + b2.x * r2;
    const Vec3 n1 = b0.y * r0 + b1.y * r1 + b2.y * r2;
    const Vec3 n2 = b0.z * r0 + b1.z * r1 + b2.z * r2;
    e[0] = n0.x; e[1] = n0.y; e[2] = n0.z;
    e[4] = n1.x; e[5] = n1.y; e[6] = n1.z;
    e[8] = n2.x; e[9] = n2.y; e[10] = n2.z;
    const Vec3 dpos = (s0 * g_pose_pos.x) * r0 + (s1 * g_pose_pos.y) * r1 +
                      (s2 * -g_pose_pos.z) * r2;
    math::store3(e + 12, cpos + dpos * view_rewrite::world_scale());
    remember_rewrite(e);

    g_rewrites++;
    g_rewrite_frame = hooks::frame_count();
    if (st) {
        st->rewrites++;
    }
}

} // namespace

// Must be OUTSIDE the anonymous namespace to match the header declaration.
// Valid only when a rewrite ACTUALLY happened this game frame (hmd_delta's
// per-eye delta must never ride on records whose pose never got the union).
bool get_union(math::Quat *rot, math::Vec3 *pos)
{
    const uint64_t frame = hooks::frame_count();
    if (!g_enabled || !g_pose_valid || g_pose_frame != frame ||
        g_rewrite_frame != frame) {
        return false;
    }
    *rot = g_pose_rot;
    *pos = g_pose_pos;
    return true;
}

// Staleness window, NOT same-frame: the HMD FOV is a property of the optics
// (quasi-static), and the view-context builder may run on another thread or
// before this frame's first fill.
bool get_fov_union(float *half_h, float *half_v)
{
    if (!(g_fov_half_h > 0.0f) || !(g_fov_half_v > 0.0f) || g_pose_ms == 0 ||
        GetTickCount64() - g_pose_ms > 1000) {
        return false;
    }
    *half_h = g_fov_half_h;
    *half_v = g_fov_half_v;
    return true;
}

bool is_union_camera(const float *rows, const float *pos)
{
    // Exact copies in practice; the tolerance only absorbs float re-encoding
    // (rows are unit vectors; positions are world metres).
    constexpr float ROW_EPS = 1e-3f, POS_EPS = 1e-2f;
    const uint64_t now = hooks::frame_count();
    for (Rewritten &h : g_history) {
        const uint32_t s0 = h.seq.load(std::memory_order_acquire);
        if (s0 == 0 || (s0 & 1u) != 0) {
            continue;  // never written / being written
        }
        const uint64_t frame = h.frame;
        float r[9], p[3];
        memcpy(r, h.rows, sizeof(r));
        memcpy(p, h.pos, sizeof(p));
        std::atomic_thread_fence(std::memory_order_acquire);
        if (h.seq.load(std::memory_order_relaxed) != s0 || now - frame > 3) {
            continue;  // torn read or stale (> 3 frames old)
        }
        bool match = true;
        for (int i = 0; i < 3 && match; i++) {
            for (int j = 0; j < 3; j++) {
                if (std::fabs(rows[i * 4 + j] - r[i * 3 + j]) > ROW_EPS) {
                    match = false;
                    break;
                }
            }
            if (std::fabs(pos[i] - p[i]) > POS_EPS) {
                match = false;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

bool set_inject_enabled(const char *value)
{
    bool on;
    if (strcmp(value, "on") == 0) {
        on = true;
    } else if (strcmp(value, "off") == 0) {
        on = false;
    } else {
        return false;
    }
    if (on == g_enabled) {
        return true; // idempotent
    }
    g_enabled = on;
    MC2VR_LOG("camtable: union injection %s", on ? "ARMED" : "disabled");
    return true;
}

bool set_probe_enabled(const char *value)
{
    bool on;
    if (strcmp(value, "on") == 0) {
        on = true;
    } else if (strcmp(value, "off") == 0) {
        on = false;
    } else {
        return false;
    }
    if (on == g_probe) {
        return true;
    }
    g_probe = on;
    MC2VR_LOG("camtable: transfer probe %s (fixed +10° local-axis rotations, "
              "x/y/z cycling 2s each; overrides the union rewrite)",
              on ? "ARMED" : "disabled");
    return true;
}

void install()
{
    auto mid = SafetyHookMid::create(reinterpret_cast<uint8_t *>(MC2_CAMTABLE_FILL_COPY_END),
                                     fill_midhook);
    if (!mid) {
        MC2VR_LOG("camtable: fill-site MidHook install FAILED @ %p (error %u) — "
                  "union injection stays idle",
                  (void *)MC2_CAMTABLE_FILL_COPY_END, (unsigned)mid.error().type);
        return;
    }
    g_mid = std::move(*mid);
    MC2VR_LOG("camtable: installed fill-site MidHook @ %p (g_CameraTable union injection%s)",
              (void *)MC2_CAMTABLE_FILL_COPY_END, g_enabled ? ", ARMED" : ", idle");
}

void report_window()
{
    if (!g_enabled && !g_probe) {
        return;
    }
    MC2VR_LOG("camtable: window: fills=%llu rewritten=%llu noPose=%llu badEntry=%llu "
              "badRows=%llu frameWaits=%llu poseId=%u%s | poseMiss read=%llu "
              "untracked=%llu insane=%llu held=%llu",
              (unsigned long long)g_fills, (unsigned long long)g_rewrites,
              (unsigned long long)g_no_pose, (unsigned long long)g_bad_entry,
              (unsigned long long)g_bad_rows, (unsigned long long)g_frame_waits,
              g_pose_id,
              g_s0 == 0 ? " | FRAME UNLOCKED — rewrite waiting for calibration"
                        : "",
              (unsigned long long)g_miss_read, (unsigned long long)g_miss_untracked,
              (unsigned long long)g_miss_insane, (unsigned long long)g_pose_held);
    g_miss_read = g_miss_untracked = g_miss_insane = g_pose_held = 0;
    for (uint32_t k = 0; k < g_entries_n; k++) {
        MC2VR_LOG("camtable:   entry slot=%u.%u rot=%p fills=%llu rewrites=%llu",
                  g_entries[k].slot, g_entries[k].sub, (void *)g_entries[k].rot,
                  (unsigned long long)g_entries[k].fills,
                  (unsigned long long)g_entries[k].rewrites);
        g_entries[k].fills = 0;
        g_entries[k].rewrites = 0;
    }
    g_fills = 0;
    g_rewrites = 0;
    g_no_pose = 0;
    g_bad_entry = 0;
    g_bad_rows = 0;
    g_frame_waits = 0;
}

} // namespace mc2vr::camera_table
