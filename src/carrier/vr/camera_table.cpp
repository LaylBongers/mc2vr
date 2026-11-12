// Union HMD camera injection at g_CameraTable — see camera_table.hpp for the
// design and docs/camera.md for the evidence.

#include "vr/camera_table.hpp"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "game/addresses.h"
#include "game/view_context.hpp"
#include "hooks/frame_tick.hpp"
#include "log.hpp"
#include "vr/vec_math.hpp"
#include "vr/xr_session.hpp"
#include "vr/view_rewrite.hpp"
#include "vr/vp_camera.hpp"

namespace mc2vr::camera_table {

using math::Quat;
using math::Vec3;

namespace {

// ---- pose (sampled once per game frame) ----------------------------------
bool g_pose_valid = false;
uint64_t g_pose_frame = ~0ull;  // frame the pose was last sampled in
uint32_t g_pose_id = 0;
int64_t g_pose_display_time = 0;
Quat g_pose_rot;
Vec3 g_pose_pos;
uint64_t g_pose_ms = 0;  // last successful pose/fov sample (staleness window)
constexpr uint64_t kPoseHoldMs = 250;  // reuse the last good pose this long on a miss
// Why a sample failed (window counters): seqlock read gave up / host reports
// untracked / eye pose or FOV failed the sanity check; held = a miss bridged
// by the last good pose.
uint64_t g_miss_read = 0, g_miss_untracked = 0, g_miss_insane = 0, g_pose_held = 0;
// HMD FOV union (radians, outermost bound over both eyes about the mid pose's
// axes) — the cull frustum input (cull_frustum.cpp via get_fov_union).
// g_fov_cant = the larger eye's rotation away from the mid pose (radians,
// window report only: 0 on parallel-display headsets).
float g_fov_half_h = 0.0f, g_fov_half_v = 0.0f, g_fov_cant = 0.0f;

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
    uintptr_t entry = 0;  // the rewritten entry's rows address (entry+0x10)
    Quat urot;  // the union pose composed into this entry
    Vec3 upos;
    uint32_t pose_id = 0;  // its HMD sample
    int64_t display_time = 0;
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
    h.entry = reinterpret_cast<uintptr_t>(e);
    h.urot = g_pose_rot;
    h.upos = g_pose_pos;
    h.pose_id = g_pose_id;
    h.display_time = g_pose_display_time;
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
bool eye_is_sane(const xr_session::EyePose &e)
{
    const float n = math::norm_sq(Quat{e.rot.x, e.rot.y, e.rot.z, e.rot.w});
    return n >= 0.98f && n <= 1.02f && e.fov.right > e.fov.left && e.fov.up > e.fov.down;
}

// Read the published views and cache the UNION (mid-eye) pose for this frame
// (the fill loop runs on the main thread; the read never waits).
void sample_pose()
{
    const uint64_t frame = hooks::frame_count();
    if (frame == g_pose_frame) {
        return; // once per frame — all fills in a frame share one pose
    }
    g_pose_frame = frame;
    g_pose_valid = false;

    xr_session::ViewState st;
    bool ok = true;
    if (!xr_session::read_state(&st)) {
        g_miss_read++;
        ok = false;
    } else if (!st.tracked) {
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
    g_pose_id = st.pose_id;
    g_pose_display_time = st.display_time;
    g_pose_valid = true;
    g_pose_ms = GetTickCount64();

    // Cull frustum input: the FOV UNION over both eyes, as half-angles in
    // radians about the MID pose's axes (the frame the union camera renders
    // in). Each eye's FOV is relative to that eye's own orientation, which on
    // canted-display headsets is turned outward from the mid pose; so each
    // eye's four frustum-corner rays are rotated into the mid frame and the
    // outermost |x/-z|, |y/-z| over all eight is the union (a linear-fractional
    // extremum over the frustum is at a corner). Symmetric, outermost side on
    // each axis. Sanity-bounded so a garbage host value never stretches the
    // culling volume.
    float th = 0.0f, tv = 0.0f, cant = 0.0f;
    bool fov_ok = true;
    const Quat mid_inv = math::conj(g_pose_rot);
    for (int e = 0; e < 2 && fov_ok; e++) {
        const xr_session::EyePose &eye = st.eye[e];
        const Quat er{eye.rot.x, eye.rot.y, eye.rot.z, eye.rot.w};
        const Quat rel = mid_inv * er;
        cant = std::fmax(cant, 2.0f * std::acos(std::fmin(1.0f, std::fabs(rel.w))));
        const float xs[2] = {std::tan(eye.fov.left), std::tan(eye.fov.right)};
        const float ys[2] = {std::tan(eye.fov.down), std::tan(eye.fov.up)};
        for (float x : xs) {
            for (float y : ys) {
                const Vec3 d = math::rotate(rel, Vec3{x, y, -1.0f});  // OpenXR: -Z forward
                if (!(-d.z > 0.05f)) {
                    fov_ok = false;  // a corner at or behind the mid plane: not a sane HMD
                    break;
                }
                th = std::fmax(th, std::fabs(d.x / -d.z));
                tv = std::fmax(tv, std::fabs(d.y / -d.z));
            }
        }
    }
    const float hh = std::atan(th), hv = std::atan(tv);
    if (fov_ok && hh > 0.05f && hh < 1.5f && hv > 0.05f && hv < 1.5f) {
        g_fov_half_h = hh;
        g_fov_half_v = hv;
        g_fov_cant = cant;
    }
}

// ---- camera frame calibration -------------------------------------------
// Runtime verification of the entry convention (probe-derived final model:
// the rendered camera's axes are the entry's ROWS — see the composition
// block in on_entry_filled and the plate on CameraTable_FillFromPose). Measures
// sign(dot(row_j, rendered_axis_j)) against the decomposed VP camera
// (view_rewrite::get_game_camera — the physically-validated basis the record path
// renders with); two consecutive agreeing fills lock the signs. While the
// head roughly faces where the game camera points, the dots are ~+-1. The
// rewrite WAITS (counted as frameWaits) until the signs lock — a changed
// measurement between builds means the convention changed: re-measure it
// before trusting the union pose (docs/camera.md).
int8_t g_s0 = 0, g_s1 = 0, g_s2 = 0;  // measured ROW signs; 0 = unlocked
int8_t g_cand0 = 0, g_cand1 = 0, g_cand2 = 0;
uint32_t g_cand_agree = 0;
uint64_t g_frame_waits = 0;
uint64_t g_rewrite_frame = ~0ull;  // frame of the last actual rewrite

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

} // namespace

void on_entry_filled(uintptr_t rot, uintptr_t slot)
{
    g_fills++;

    // rot is always inside g_CameraTable (the fill loop is the only caller) —
    // the range check is belt-and-braces against layout surprises.
    if (rot - MC2_G_CAMTABLE >= MC2_CAMTABLE_SLOT_STRIDE * MC2_CAMTABLE_SLOTS) {
        g_bad_entry++;
        return;
    }

    // slot/sub are census labels, not injection inputs.
    uint32_t slot_idx = 0xffffffffu, sub_idx = 0xffffffffu;
    if (slot - MC2_G_CAMTABLE < MC2_CAMTABLE_SLOT_STRIDE * MC2_CAMTABLE_SLOTS) {
        slot_idx = (uint32_t)((slot - MC2_G_CAMTABLE) / MC2_CAMTABLE_SLOT_STRIDE);
        const uintptr_t off = (rot - MC2_G_CAMTABLE) % MC2_CAMTABLE_SLOT_STRIDE;
        if (off >= game::kCamEntryWorld) {
            sub_idx = (uint32_t)((off - game::kCamEntryWorld) / game::kCamEntryStride);
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

bool rewritten_entry(uintptr_t rows, UnionSample *sample, Quat *rot, Vec3 *pos)
{
    // The same entry address is rewritten every frame or two, so the history
    // holds several rewrites of it: take the NEWEST (the one the builder copied).
    const uint64_t now = hooks::frame_count();
    bool found = false;
    uint64_t best_frame = 0;
    for (Rewritten &h : g_history) {
        const uint32_t s0 = h.seq.load(std::memory_order_acquire);
        if (s0 == 0 || (s0 & 1u) != 0) {
            continue;  // never written / being written
        }
        const uint64_t frame = h.frame;
        const uintptr_t entry = h.entry;
        const UnionSample smp{h.pose_id, h.display_time};
        const Quat q = h.urot;
        const Vec3 p = h.upos;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (h.seq.load(std::memory_order_relaxed) != s0 || now - frame > 3) {
            continue;  // torn read or stale (> 3 frames old)
        }
        if (entry == rows && (!found || frame > best_frame)) {
            found = true;
            best_frame = frame;
            if (sample != nullptr) *sample = smp;
            if (rot != nullptr) *rot = q;
            if (pos != nullptr) *pos = p;
        }
    }
    return found;
}

void report_window()
{
    MC2VR_LOG("camtable: window: fills=%llu rewritten=%llu noPose=%llu badEntry=%llu "
              "badRows=%llu frameWaits=%llu poseId=%u%s | poseMiss read=%llu "
              "untracked=%llu insane=%llu held=%llu | fov union %.1f°/%.1f° cant %.2f°",
              (unsigned long long)g_fills, (unsigned long long)g_rewrites,
              (unsigned long long)g_no_pose, (unsigned long long)g_bad_entry,
              (unsigned long long)g_bad_rows, (unsigned long long)g_frame_waits,
              g_pose_id,
              g_s0 == 0 ? " | FRAME UNLOCKED — rewrite waiting for calibration"
                        : "",
              (unsigned long long)g_miss_read, (unsigned long long)g_miss_untracked,
              (unsigned long long)g_miss_insane, (unsigned long long)g_pose_held,
              (double)g_fov_half_h * 57.29577951, (double)g_fov_half_v * 57.29577951,
              (double)g_fov_cant * 57.29577951);
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
