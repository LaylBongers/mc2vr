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
uint64_t g_pose_ms = 0;  // last successful pose/fov sample (staleness window)

// ---- camera-entry fov channel (frustum_cull_plan.md, ADS clue) ----------
// The camera entry's fov +0x58 (=300.0 from a static constant) feeds the
// projection (BuildCameraConstants tan-table) AND the 0x0048067E cull-reader
// family. ADS narrows culling (user report) while the ViewEntry fov sources
// stay constant — the entry chain is the prime operative-cull suspect. The
// fill hook logs the census (which entries, what fov) and optionally scales
// the fov value (entry_fov_scale) as a causal probe.
// The causal SCALE at the SOURCE: patch the game's fov constant itself —
// all 8 readers (both camera-entry fillers incl. the stack-local one the
// fill hook misses, AND the 0x0070axx cull-fov derivations) pick it up
// in the game's own parametrization. One write at init; the game never
// rewrites the constant.
// DECOUPLE (entry_fov_decouple): the constant feeds BOTH the cull (readers
// FUN_0070aa60/0x0070a910/0x0070aff6 — the operative channel, water-proven)
// AND the game's own systems via the camera-entry fillers (+0x58 →
// projection + camera-boom logic — which breaks at wide scales: the camera
// pulls in and offscreen passes artifact, live 2026-10-08 scale 2.3). Fix:
// repoint the THREE MOVSS XMM0,[0x00BEAB5C] loads in the fillers (imm32
// operands at 0x00466105 / 0x0046618C / 0x004665FF — FUN_004660a6 x2 +
// CamPose_CopyGlobal_To_Entry) at g_ConstPool's 1.0 (0x00B9B694). The
// fillers then write multiplier 1.0 (game projection/camera stay sane)
// while the cull derivations keep reading the patched-wide constant.
// Evidence this splits correctly: round 5 (entries scaled, constant NOT)
// moved NEITHER gameproj NOR water; round 7 (constant scaled) moved BOTH —
// the water cull follows the CONSTANT readers, the projection follows the
// FILLERS.
float g_entry_fov_scale = 1.0f;  // NOT bool — a bool-typed decl silently
                                 // collapsed the scale to 1 and no-op'd the
                                 // whole round-8 run (live lesson 2026-10-08)
bool g_entry_fov_decouple = false;
float g_entry_fov_last = 0.0f;
uint64_t g_entry_fills = 0, g_entry_fov_scaled = 0;
constexpr uint32_t FOV_CONST = 0x00beab5cu;
constexpr uint32_t CONST_STOCK_FOV = 0x00bad260u;  // .rdata 300.0f — the stock
// post-boot fov value. Round 8 v2 LESSON: the fillers write the loaded value
// RAW into entry+0x58, so the decouple must load 300.0 (stock), NOT 1.0 —
// a 1.0 put the game projection at ~0.08deg half-angle = extreme telephoto =
// "camera incredibly close" (live 2026-10-08).

bool patch_imm32s(const uintptr_t *addrs, uint32_t n, uint32_t from, uint32_t to,
                  const char *what)
{
    bool ok = true;
    uint32_t patched = 0;
    for (uint32_t k = 0; k < n; k++) {
        uint32_t cur;
        memcpy(&cur, (const void *)addrs[k], 4);
        if (cur != from) {
            ok = false;
            MC2VR_LOG("camtable: %s site %p = %08X (expected %08X) — SKIPPED",
                      what, (void *)addrs[k], cur, from);
            continue;
        }
        DWORD oldp = 0;
        if (!VirtualProtect((void *)addrs[k], 4, PAGE_READWRITE, &oldp)) {
            ok = false;
            MC2VR_LOG("camtable: %s site %p VirtualProtect FAILED (gle %u) — SKIPPED",
                      what, (void *)addrs[k], (unsigned)GetLastError());
            continue;
        }
        memcpy((void *)addrs[k], &to, 4);
        VirtualProtect((void *)addrs[k], 4, oldp, &oldp);
        patched++;
    }
    MC2VR_LOG("camtable: %s %s (%u/%u sites)", what,
              ok ? "PATCHED" : "PARTIAL", patched, n);
    return ok;
}

void install_decouple()
{
    // ROUND 9 MODE ("statics wide", after the v5 dead end): ALL 8 constant
    // readers turned out to be FILLERS (no separate cull-derivation readers
    // exist) — FUN_0070aff6 templates THREE STACK camera entries (the ones
    // the projection's active entry comes from — proven v5: leaving it wide
    // kept gameproj at 84.33deg with 5/5 other sites patched). So the cull
    // and the projection read the same ENTRY fields, and the only remaining
    // split is WHICH ENTRY: the cull may read the STATIC g_CameraTable
    // entries (E2b: "the culling/fov readers 0x0048067E read the same
    // [g_CameraTable]"), while the projection reads the STACK entries.
    // This mode: patch every NON-CamPose filler load -> stock 300 (stack/aux
    // entries + the projection stay stock), while CamPose_CopyGlobal_To_Entry
    // (load imm32 @ 0x004665FF) keeps reading the wide-patched constant —
    // the STATIC entries go wide. If the cull follows the statics, culling
    // widens while the game renders stock.
    //   CamPose: NOT patched (statics wide). Others (raw-verified 2026-10-08,
    //   all `F3 0F 10 05 5C AB BE 00`, imm32 = opcode+4):
    //   0x00466101/@05, 0x00466188/@8C (FUN_004660a6, two entries)
    //   0x0070aa86/@8A (FUN_0070aa60), 0x0070a94b/@4F (FUN_0070a910 — v4's
    //   six-byte miscount live-caught by the value guard)
    //   0x0070b039/@3D, 0x0070b0bd/@C1, 0x0070b144/@48 (FUN_0070aff6's three
    //   STACK-entry templates)
    static const uintptr_t loads[7] = {0x00466105u, 0x0046618cu, 0x0070aa8au,
                                       0x0070a94fu, 0x0070b03du, 0x0070b0c1u,
                                       0x0070b148u};
    patch_imm32s(loads, 7, FOV_CONST, CONST_STOCK_FOV,
                 "fov DECOUPLE v2: non-CamPose fillers -> 300.0 stock (statics stay WIDE)");
}
struct EntrySeen {
    uintptr_t va;
    float near_v, far_v, fov;
    uint64_t count;
};
constexpr uint32_t ENTRIES_FOV_MAX = 8;
EntrySeen g_entry_fov_seen[ENTRIES_FOV_MAX];
uint32_t g_entry_fov_seen_n = 0;
SafetyHookMid g_entry_mid;

void entry_midhook(safetyhook::Context &ctx)
{
    const uintptr_t entry = ctx.esi;
    float fov, near_v, far_v;
    memcpy(&fov, (const void *)(entry + MC2_VCCAM_ENTRY_FOVCOS_OFF), 4);
    memcpy(&near_v, (const void *)(entry + MC2_VCCAM_ENTRY_NEAR_OFF), 4);
    memcpy(&far_v, (const void *)(entry + MC2_VCCAM_ENTRY_FAR_OFF), 4);

    // Census: log each distinct entry once and any fov change (bounded).
    // (The causal SCALE moved to the SOURCE constant — see install(): this
    // hook is census-only now.)
    g_entry_fills++;
    EntrySeen *slot = nullptr;
    for (uint32_t k = 0; k < g_entry_fov_seen_n; k++) {
        if (g_entry_fov_seen[k].va == entry) {
            slot = &g_entry_fov_seen[k];
            break;
        }
    }
    if (slot == nullptr && g_entry_fov_seen_n < ENTRIES_FOV_MAX) {
        slot = &g_entry_fov_seen[g_entry_fov_seen_n++];
        *slot = {entry, near_v, far_v, fov, 0};
        MC2VR_LOG("camtable: entry %p first seen: near=%.3f far=%.1f fov=%.1f",
                  (void *)entry, (double)near_v, (double)far_v, (double)fov);
    }
    if (slot) {
        slot->count++;
        if (fov != slot->fov) {
            MC2VR_LOG("camtable: entry %p FOV CHANGE %.1f -> %.1f (near=%.3f far=%.1f)",
                      (void *)entry, (double)slot->fov, (double)fov,
                      (double)near_v, (double)far_v);
            slot->fov = fov;
            slot->near_v = near_v;
            slot->far_v = far_v;
        }
    }
    if (g_entry_fov_last == 0.0f && fov != 0.0f) {
        g_entry_fov_last = fov;
    }
}
uint64_t g_fills = 0, g_rewrites = 0;
uint64_t g_no_pose = 0, g_bad_entry = 0, g_bad_rows = 0;

// ---- cull fov widening (frustum_cull_plan.md D0) ---------------------------
bool g_fov_widen = false;
float g_fov_margin_deg = 5.0f;
float g_fov_half_h = 0.0f, g_fov_half_v = 0.0f; // HMD FOV-union half-angles (rad)
uint64_t g_fov_widens = 0, g_fov_skips = 0, g_fov_bad_gcam = 0;
float g_fov_last_wh = 0.0f, g_fov_last_wv = 0.0f;
SafetyHookMid g_fov_mid;

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
    g_pose_ms = GetTickCount64();

    // Cull-fov widening input: the FOV UNION over both eyes — half-angles in
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
// (view::get_game_camera — the physically-validated basis the record path
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
        vpcam::Camera cam;
        char dec[192];
        int m = 0;
        if (view::get_game_camera(&cam)) {
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
    vpcam::Camera cam;
    if (!view::get_game_camera(&cam)) {
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

// Cull-fov widening hook (frustum_cull_plan.md D0). Site 0x0048a97a: the
// fallback (kindA4==0 — the LIVE path) just wrote {fovH*k, 0, fovV*k} at
// EDI = entry+0x2ec, where {fovH,fovV} is the game's unit frustum-corner
// direction (fovV/fovH = tanV/tanH, fovH^2+fovV^2 = 1; live 2026-10-08:
// 0.957826 / 0.287348 — 3:1 tan shape) and k = gcamScale*100 (near dist).
// Dividing by the game's own corner and multiplying the HMD union's tan
// half-angles (+ margin) replaces the cull cross-section with the HMD FOV
// in exactly the game's parametrization — correct whether the consumer
// treats the triple as tans or a scaled direction (Vector_Scale in the
// cull task scales it out to the cull bounds either way). The length sqrt
// right after the hook re-reads the scaled memory, so the derived near
// distance stays consistent.
void fov_midhook(safetyhook::Context &ctx)
{
    if (!g_fov_widen) {
        return;
    }
    // Once-per-frame pose/fov sample (shared with the fill hook; works with
    // or without view_table_inject). Without a tracked HMD this bails and the
    // triple keeps the game's own fov.
    sample_pose();
    if (!g_pose_valid || !(g_fov_half_h > 0.0f) || !(g_fov_half_v > 0.0f)) {
        g_fov_skips++;
        return;
    }
    const uint32_t owner = *(const uint32_t *)MC2_G_GLOBALCAM_OWNER;
    const uint32_t gcam = owner ? *(const uint32_t *)(uintptr_t)(owner + MC2_GLOBALCAM_OBJ_OFF) : 0;
    if (gcam == 0) {
        g_fov_bad_gcam++;
        return;
    }
    float h, v;
    memcpy(&h, (const void *)(uintptr_t)(gcam + MC2_GLOBALCAM_FOV_H_OFF), 4);
    memcpy(&v, (const void *)(uintptr_t)(gcam + MC2_GLOBALCAM_FOV_V_OFF), 4);
    // h,v > 0 also rejects NaN; zero/negative = transient garbage — skip
    // (the triple keeps the game value, which is always safe).
    if (!(h > 0.0f) || !(v > 0.0f)) {
        g_fov_bad_gcam++;
        return;
    }
    const float margin = g_fov_margin_deg * 0.017453293f;
    const float wh = std::tan(g_fov_half_h + margin) / h;
    const float wv = std::tan(g_fov_half_v + margin) / v;
    float *triple = (float *)(uintptr_t)(uint32_t)ctx.edi;
    triple[0] *= wh;
    triple[2] *= wv;
    g_fov_widens++;
    g_fov_last_wh = wh;
    g_fov_last_wv = wv;
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
    math::store3(e + 12, cpos + dpos * view::world_scale());

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

// Cull-fov inputs for other modules (record_fov_widen): the HMD FOV-union
// half-angles + cull_fov_margin. STALENESS WINDOW, NOT same-frame: the
// record builds run BEFORE the camtable fill's pose sample in the frame
// order (live evidence 2026-10-08: the same-frame gate zeroed every widen
// attempt — recfov widens=0 across a whole run), and the FOV is quasi-static
// (a property of the HMD optics). Only the POSE needs same-frame pairing,
// and hmd_delta handles that via get_union.
bool get_fov_union(float *half_h, float *half_v)
{
    if (!(g_fov_half_h > 0.0f) || !(g_fov_half_v > 0.0f) ||
        g_pose_ms == 0 || GetTickCount64() - g_pose_ms > 1000) {
        return false;
    }
    const float margin = g_fov_margin_deg * 0.017453293f;
    *half_h = g_fov_half_h + margin;
    *half_v = g_fov_half_v + margin;
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

bool set_fov_widen(const char *value)
{
    bool on;
    if (strcmp(value, "on") == 0) {
        on = true;
    } else if (strcmp(value, "off") == 0) {
        on = false;
    } else {
        return false;
    }
    if (on == g_fov_widen) {
        return true; // idempotent
    }
    g_fov_widen = on;
    MC2VR_LOG("camtable: cull fov widening %s (HMD FOV union + %.1f° margin at "
              "the entry fov triple)",
              on ? "ARMED" : "disabled", (double)g_fov_margin_deg);
    return true;
}

bool set_fov_margin(double degrees)
{
    if (!(degrees >= 0.0) || degrees >= 45.0) {
        MC2VR_LOG("camtable: cull_fov_margin=%.3f out of range [0,45) — kept %.1f",
                  degrees, (double)g_fov_margin_deg);
        return false;
    }
    g_fov_margin_deg = (float)degrees;
    MC2VR_LOG("camtable: cull fov margin = %.1f°", degrees);
    return true;
}

// entry_fov_scale: causal probe on the camera-entry fov value (+0x58).
// 1.0 = observe only; >1 widens (value feeds the projection tan-table index
// AND the 0x0048067E cull readers — units unknown, index ∝ value ≈ angle).
bool set_entry_fov_scale(double scale)
{
    if (!(scale > 0.0) || scale > 10.0) {
        MC2VR_LOG("camtable: entry_fov_scale=%.3f out of range (0,10] — kept %.3f",
                  scale, (double)g_entry_fov_scale);
        return false;
    }
    g_entry_fov_scale = (float)scale;
    MC2VR_LOG("camtable: entry fov scale = %.3f (%s)", scale,
              scale == 1.0f ? "observe only" : "SCALING the fov constant");
    return true;
}

// entry_fov_decouple: with entry_fov_scale active, repoint the camera-entry
// fillers' fov-constant loads at 1.0 so the game's own projection/camera
// stay at the stock fov while the cull derivations read the wide constant.
bool set_entry_fov_decouple(const char *value)
{
    bool on;
    if (strcmp(value, "on") == 0) {
        on = true;
    } else if (strcmp(value, "off") == 0) {
        on = false;
    } else {
        return false;
    }
    if (on == g_entry_fov_decouple) {
        return true;
    }
    g_entry_fov_decouple = on;
    MC2VR_LOG("camtable: entry fov decouple %s (filler loads -> 300.0 stock; cull "
              "readers keep the wide constant)",
              on ? "ON" : "off");
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

    // Fov-write MidHook (cull_fov_widen): installed always, the handler gates
    // on the conf — so a conf flip in a future conf-reload path would just
    // start working, and the hook is inert otherwise.
    auto fmid = SafetyHookMid::create(reinterpret_cast<uint8_t *>(MC2_VIEWENTRY_FOV_WRITE_END),
                                     fov_midhook);
    if (!fmid) {
        MC2VR_LOG("camtable: fov-write MidHook install FAILED @ %p (error %u) — "
                  "cull fov widening stays idle",
                  (void *)MC2_VIEWENTRY_FOV_WRITE_END, (unsigned)fmid.error().type);
        return;
    }
    g_fov_mid = std::move(*fmid);
    MC2VR_LOG("camtable: installed fov-write MidHook @ %p (cull fov widening%s)",
              (void *)MC2_VIEWENTRY_FOV_WRITE_END, g_fov_widen ? ", ARMED" : ", idle");

    // Camera-entry fill epilogue: entry fov census + causal scale probe
    // (frustum_cull_plan.md — the ADS-narrowing clue implicates this chain).
    auto emid = SafetyHookMid::create(reinterpret_cast<uint8_t *>(MC2_CAMENTRY_FILL_EPILOG),
                                      entry_midhook);
    if (!emid) {
        MC2VR_LOG("camtable: entry-fill MidHook install FAILED @ %p (error %u) — "
                  "entry fov census stays idle",
                  (void *)MC2_CAMENTRY_FILL_EPILOG, (unsigned)emid.error().type);
        return;
    }
    g_entry_mid = std::move(*emid);
    MC2VR_LOG("camtable: installed entry-fill MidHook @ %p (entry fov census%s)",
              (void *)MC2_CAMENTRY_FILL_EPILOG,
              g_entry_fov_scale != 1.0f ? ", SCALING" : ", observe");

    // The causal SCALE at the SOURCE: patch the game's fov constant itself —
    // all 8 readers (both camera-entry fillers incl. the stack-local one the
    // fill hook misses, AND the 0x0070axx cull-fov derivations) pick it up
    // in the game's own parametrization. One write at init; the game never
    // rewrites the constant.
    if (g_entry_fov_scale != 1.0f) {
        // The constant lives in .rdata (0x00b05000-0x00bf4fff — READ-ONLY:
        // writing it unguarded crashed the game at startup, live-proven
        // 2026-10-08 round 6). Flip the page writable for the one write.
        DWORD oldProtect = 0;
        float orig;
        memcpy(&orig, (const void *)MC2_CAMENTRY_FOV_CONST, 4);
        if (orig > 0.0f && std::isfinite(orig) && orig < 10000.0f &&
            VirtualProtect((void *)MC2_CAMENTRY_FOV_CONST, 4, PAGE_READWRITE,
                           &oldProtect)) {
            const float scaled = orig * g_entry_fov_scale;
            memcpy((void *)MC2_CAMENTRY_FOV_CONST, &scaled, 4);
            VirtualProtect((void *)MC2_CAMENTRY_FOV_CONST, 4, oldProtect, &oldProtect);
            MC2VR_LOG("camtable: fov CONSTANT PATCHED @ %p: %.1f -> %.1f "
                      "(x%.3f — the whole fov chain: entries, cull derivations)",
                      (void *)MC2_CAMENTRY_FOV_CONST,
                      (double)orig, (double)scaled, (double)g_entry_fov_scale);
        } else {
            MC2VR_LOG("camtable: fov constant @ %p = %.1f — unexpected value or "
                      "VirtualProtect failed, NOT patched",
                      (void *)MC2_CAMENTRY_FOV_CONST, (double)orig);
        }
        if (g_entry_fov_decouple) {
            install_decouple();
        }
    }
}

void report_window()
{
    if (!g_enabled && !g_probe && !g_fov_widen && g_entry_fills == 0) {
        return;
    }
    MC2VR_LOG("camtable: window: fills=%llu rewritten=%llu noPose=%llu badEntry=%llu "
              "badRows=%llu frameWaits=%llu poseId=%u%s",
              (unsigned long long)g_fills, (unsigned long long)g_rewrites,
              (unsigned long long)g_no_pose, (unsigned long long)g_bad_entry,
              (unsigned long long)g_bad_rows, (unsigned long long)g_frame_waits,
              g_pose_id,
              g_s0 == 0 ? " | FRAME UNLOCKED — rewrite waiting for calibration"
                        : "");
    // Fov probe (frustum_cull_plan.md open item): log the global-cam fov
    // SOURCES next to the entry triple they produce, once per window — a
    // live run that zooms/changes the game FOV option then shows whether
    // fovH/fovV/scale track tan(half-angle) (expected per the static decode)
    // and which angle is which. Reads run on the poller thread, like the
    // watch value snapshot — pointers guarded, best-effort.
    const uint32_t owner = *(const uint32_t *)MC2_G_GLOBALCAM_OWNER;
    if (owner != 0) {
        const uint32_t gcam = *(const uint32_t *)(owner + MC2_GLOBALCAM_OBJ_OFF);
        if (gcam != 0) {
            auto rdf = [](uintptr_t a) {
                float f;
                memcpy(&f, (const void *)a, 4);
                return f;
            };
            const float h = rdf(gcam + MC2_GLOBALCAM_FOV_H_OFF);
            const float v = rdf(gcam + MC2_GLOBALCAM_FOV_V_OFF);
            const float s = rdf(gcam + MC2_GLOBALCAM_FOV_SCALE_OFF);
            MC2VR_LOG("camtable: fovsrc: gcam=%08X h=%.6g v=%.6g scale=%.6g"
                      " -> entry triple {%.6g, 0, %.6g} (half-angle if tan:"
                      " h=%.2fdeg v=%.2fdeg)",
                      gcam, h, v, s, h * s * 100.0f, v * s * 100.0f,
                      std::atan(h * s * 100.0f) * 180.0f / 3.14159265f,
                      std::atan(v * s * 100.0f) * 180.0f / 3.14159265f);
        }
    }
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

    if (g_fov_widen || g_fov_widens != 0) {
        MC2VR_LOG("camtable: fovwiden: widens=%llu skips=%llu badGcam=%llu "
                  "lastWh=%.3f lastWv=%.3f",
                  (unsigned long long)g_fov_widens, (unsigned long long)g_fov_skips,
                  (unsigned long long)g_fov_bad_gcam,
                  (double)g_fov_last_wh, (double)g_fov_last_wv);
        g_fov_widens = 0;
        g_fov_skips = 0;
        g_fov_bad_gcam = 0;
    }
}

} // namespace mc2vr::camtable
