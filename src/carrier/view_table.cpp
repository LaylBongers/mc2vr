// Union HMD camera injection at g_CameraTable — see view_table.hpp for the
// design and docs/stereo_improvements_plan.md for the evidence.

#include "view_table.hpp"

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
#include "view_rewrite.hpp"

namespace mc2vr::camtable {

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

// ---- window census ---------------------------------------------------------
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
    if (!ipc::read_state(&st) || !(st.flags & MC2VR_IPC_STF_TRACKED) ||
        !eye_is_sane(st.eye[0]) || !eye_is_sane(st.eye[1])) {
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
}

void fill_midhook(safetyhook::Context &ctx)
{
    if (!g_enabled) {
        return;
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

    sample_pose();
    if (!g_pose_valid) {
        g_no_pose++;
        return;
    }

    // rows = entry+0x10: the camera-to-WORLD transform E. Builder-proven
    // 2026-10-07 (first live run inverted head direction — the E2b probe's
    // fabsf(dot) "rows = axes" reading was wrong): ViewContext_BuildCamera
    // Constants copies the entry rot+pos verbatim into ctx+0xaa0, then
    // INVERTS it (FUN_008225c0 = 4x4 inverse) into the view before the
    // view*proj multiply. Row-major E: COLUMN j = camera local axis j in
    // world coords (c0 = right, c1 = up, c2 = BACKWARD — the decomposed VP
    // camera's F = -c2); rows+0x30 (entry+0x40) = world position C. The
    // game's local frame matches XR LOCAL (x right, y up, +z backward), so
    // XR coords map in with NO sign flips.
    float *e = (float *)rot;
    const Vec3 c0 = {e[0], e[4], e[8]};
    const Vec3 c1 = {e[1], e[5], e[9]};
    const Vec3 c2 = {e[2], e[6], e[10]};
    const Vec3 cpos = math::load3(e + 12);
    auto unit = [](Vec3 v) {
        const float n = math::length(v);
        return n >= 0.5f && n <= 2.0f;
    };
    if (!unit(c0) || !unit(c1) || !unit(c2)) {
        // Not an orthonormal rotation — a layout surprise, never a half-pose.
        g_bad_rows++;
        return;
    }

    // Union rotation composed on the LOCAL side: E' = E * M(q_hmd), i.e.
    // each new column = E applied to rotate(q_hmd, e_j). Position =
    // C + E*(union pos), scaled by view_world_scale. Row w components are
    // left untouched (0 / 1.0).
    auto to_world = [&](Vec3 v) { return c0 * v.x + c1 * v.y + c2 * v.z; };
    const Vec3 n0 = to_world(math::rotate(g_pose_rot, {1, 0, 0}));
    const Vec3 n1 = to_world(math::rotate(g_pose_rot, {0, 1, 0}));
    const Vec3 n2 = to_world(math::rotate(g_pose_rot, {0, 0, 1}));
    e[0] = n0.x; e[4] = n0.y; e[8] = n0.z;
    e[1] = n1.x; e[5] = n1.y; e[9] = n1.z;
    e[2] = n2.x; e[6] = n2.y; e[10] = n2.z;
    math::store3(e + 12, cpos + to_world(g_pose_pos) * view::world_scale());

    g_rewrites++;
    if (st) {
        st->rewrites++;
    }
}

} // namespace

// Must be OUTSIDE the anonymous namespace to match the header declaration.
bool get_union(math::Quat *rot, math::Vec3 *pos)
{
    if (!g_enabled || !g_pose_valid || g_pose_frame != hooks::frame_count()) {
        return false;
    }
    *rot = g_pose_rot;
    *pos = g_pose_pos;
    return true;
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
    if (!g_enabled) {
        return;
    }
    MC2VR_LOG("camtable: window: fills=%llu rewritten=%llu noPose=%llu badEntry=%llu "
              "badRows=%llu poseId=%u",
              (unsigned long long)g_fills, (unsigned long long)g_rewrites,
              (unsigned long long)g_no_pose, (unsigned long long)g_bad_entry,
              (unsigned long long)g_bad_rows, g_pose_id);
    for (uint32_t k = 0; k < g_entries_n; k++) {
        MC2VR_LOG("camtable:   entry slot=%u.%u rot=%p fills=%llu rewrites=%llu",
                  g_entries[k].slot, g_entries[k].sub, (void *)g_entries[k].rot,
                  (unsigned long long)g_entries[k].fills,
                  (unsigned long long)g_entries[k].rewrites);
        g_entries[k].fills = 0;
        g_entries[k].rewrites = 0;
    }
    if (view::full_pose_rewrite_active()) {
        MC2VR_LOG("camtable: WARNING — view_row_rewrite=hmd re-applies the FULL HMD pose "
                  "at the upload on top of this table union (rotation+translation "
                  "doubled); use hmd_delta (the intended pairing: per-eye FOV + "
                  "IPD delta only) or stereo/hmd_identity");
    }
    g_fills = 0;
    g_rewrites = 0;
    g_no_pose = 0;
    g_bad_entry = 0;
    g_bad_rows = 0;
}

} // namespace mc2vr::camtable
