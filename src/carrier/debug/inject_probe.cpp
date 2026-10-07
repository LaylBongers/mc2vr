// E2 probe implementation — see inject_probe.hpp for the experiment design.

#include "inject_probe.hpp"

#include <safetyhook.hpp>

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "game_addresses.h"
#include "hooks.hpp"
#include "log.hpp"
#include "vec_math.hpp"
#include "view_rewrite.hpp"
#include "vp_camera.hpp"

namespace mc2vr::injectprobe {

using math::Vec3;

namespace {

bool g_enabled = false;
float g_amp = 8.0f;    // world units
float g_hz = 0.25f;

// ---- E2b: builder-entry instrumentation ----
bool g_cambuilder_enabled = false;
SafetyHookMid g_cambuilder_mid;
struct CambuilderSeen {
    uintptr_t entry;
    uint32_t idx;
};
CambuilderSeen g_cambuilder_seen[24];
uint32_t g_cambuilder_seen_n = 0;
uint32_t g_cambuilder_arg_logged = 0;  // distinct ctx args seen (log first few)

void on_cambuilder(safetyhook::Context &ctx)
{
    // Arg = [ebp+8] AT ENTRY (the function uses the caller's EBP — mutated
    // convention; see game_addresses.h). Camera object = [arg+0x28], a
    // self-indexed 0x70-stride array: active entry = base + [base]*0x70.
    const uintptr_t arg =
        *(const uintptr_t *)(uintptr_t)(ctx.ebp + MC2_VCCAM_ARG_EBP_OFF);
    if (arg == 0 || arg < 0x00400000u || arg > 0x80000000u) {
        return;
    }
    const uintptr_t arr =
        *(const uintptr_t *)(arg + MC2_VCCAM_CTX_CAMARRAY_OFF);
    if (arr == 0 || arr < 0x00400000u || arr > 0x80000000u) {
        return;
    }
    const uint32_t idx = *(const uint32_t *)(uintptr_t)arr;
    if (idx > 0x1000u) {
        return;
    }
    const uintptr_t entry = arr + (uintptr_t)idx * MC2_VCCAM_ENTRY_STRIDE;

    for (uint32_t k = 0; k < g_cambuilder_seen_n; k++) {
        if (g_cambuilder_seen[k].entry == entry) {
            return;  // already dumped
        }
    }
    if (g_cambuilder_seen_n >= sizeof(g_cambuilder_seen) / sizeof(g_cambuilder_seen[0])) {
        return;
    }
    g_cambuilder_seen[g_cambuilder_seen_n++] = {entry, idx};

    // Dump the active entry (0x70 bytes = 7 rows of 4 floats). Basis hints:
    // if the entry starts with a view matrix, rows 0/2 should align with the
    // decomposed game camera's right/forward (handedness unknown — |dot|).
    const float *m = (const float *)(uintptr_t)entry;
    char dots[96];
    int n = 0;
    vpcam::Camera cam;
    if (view::get_game_camera(&cam)) {
        Vec3 r0, r2;
        const bool ok0 = math::normalize(Vec3{m[0], m[1], m[2]}, &r0);
        const bool ok2 = math::normalize(Vec3{m[8], m[9], m[10]}, &r2);
        n = _snprintf(dots, sizeof(dots), " r0.R=%.3f r2.F=%.3f",
                      ok0 ? (double)fabsf(math::dot(r0, cam.R)) : -1.0,
                      ok2 ? (double)fabsf(math::dot(r2, cam.F)) : -1.0);
    }
    dots[n > 0 ? n : 0] = '\0';
    MC2VR_LOG("cambuilder: camObj ctx=%p arr=%p idx=%u entry=%p%s",
              (void *)arg, (void *)arr, idx, (void *)entry, dots);
    MC2VR_LOG("cambuilder: entry %p rows:", (void *)entry);
    for (uint32_t row = 0; row < 7; row++) {
        MC2VR_LOG("cambuilder:   [%u] %11.3f %11.3f %11.3f %11.3f", row,
                  (double)m[row * 4 + 0], (double)m[row * 4 + 1],
                  (double)m[row * 4 + 2], (double)m[row * 4 + 3]);
    }
    // The near/far/fov floats per the decompile (entry+0x50/0x54/0x58, +0x74).
    MC2VR_LOG("cambuilder:   fields: +0x50=%.4f +0x54=%.4f +0x58=%.4f +0x74=%.4f",
              (double)m[20], (double)m[21], (double)m[22], (double)m[29]);
}

constexpr float DOT_MIN = 0.99f;       // slot0 basis vs decomposed game camera
constexpr float POS_TOL = 100.0f;     // entry pos7c4 vs decomposed camC, world units
constexpr size_t MATCH_MAX = 16;      // inject into ALL matched views (S6 D1)

// Per-window closest-live-view diagnostic (steady-state ground truth).
struct BestView {
    bool valid;
    float d;
    uint32_t idx;
    Vec3 pos;
    uint32_t live_views;
    uint32_t within_tol;
};
BestView g_best = {};

struct Matched {
    const uint8_t *entry;
    uint32_t idx;
    Vec3 base;        // game pos we inject relative to
    Vec3 last_write;  // what we wrote last (detects external refresh)
    uint64_t last_frame;
    uint64_t injects;
    uint64_t refreshes;
    bool logged;       // one-shot per matched view
};
Matched g_match[MATCH_MAX];
size_t g_match_n = 0;

uint32_t g_dump_n = 0;  // one-shot layout diagnostics (first live views)
uint64_t g_no_cam_logged = 0;  // one-shot-ish complaints
float g_follow_x = 0, g_follow_y = 0, g_follow_z = 0;  // last window sample
Vec3 g_offset_last = {0, 0, 0};
uint64_t g_injects_window = 0;
uint64_t g_refreshes_window = 0;
// Verdict regression (per window): slope = Σ(off·(cam_now−base)) / Σ|off|² —
// ≈1 = the decomposed draw camera carries our offset, ≈0 = it does not.
// Immune to the offset/window sampling phase, unlike the FOLLOW snapshot.
double g_sxy = 0, g_sxx = 0;

Vec3 read_v3(const uint8_t *p)
{
    const float *f = (const float *)p;
    return {f[0], f[1], f[2]};
}

void write_v3(uint8_t *p, Vec3 v)
{
    float *f = (float *)p;
    f[0] = v.x;
    f[1] = v.y;
    f[2] = v.z;
}
}  // namespace

bool set_entry_inject_enabled(const char *value)
{
    const bool on = strcmp(value, "on") == 0;
    const bool off = strcmp(value, "off") == 0;
    if (!on && !off) {
        return false;
    }
    g_enabled = on;
    MC2VR_LOG("inject: probe %s (pos oscillation +A*right*sin(2pi*%.3g*t), serial "
              "bump per frame; requires view_row_rewrite=hmd_delta (or "
              "hmd_identity) for matching)",
              on ? "ARMED" : "disabled", (double)g_hz);
    return true;
}

bool set_cambuilder_enabled(const char *value)
{
    const bool on = strcmp(value, "on") == 0;
    const bool off = strcmp(value, "off") == 0;
    if (!on && !off) {
        return false;
    }
    g_cambuilder_enabled = on;
    MC2VR_LOG("cambuilder: builder-entry instrumentation %s (logs the camera object "
              "+ dumps the active entry once per distinct address)",
              on ? "ON" : "off");
    return true;
}

void install()
{
    if (!g_cambuilder_enabled) {
        return;
    }
    auto mid = SafetyHookMid::create(
        reinterpret_cast<uint8_t *>(MC2_VCCAMERA_BUILDER), on_cambuilder);
    if (!mid) {
        MC2VR_LOG("cambuilder: builder MidHook install failed @ %p (error %u) — skipped",
                  (void *)MC2_VCCAMERA_BUILDER, (unsigned)mid.error().type);
        return;
    }
    g_cambuilder_mid = std::move(*mid);
    MC2VR_LOG("cambuilder: builder MidHook installed @ %p", (void *)MC2_VCCAMERA_BUILDER);
}

void set_entry_inject_offset(float units)
{
    if (units > 0.0f && units <= 100.0f) {
        g_amp = units;
        MC2VR_LOG("inject: offset amplitude = %g world units", (double)units);
    } else {
        MC2VR_LOG("inject: debug_entry_inject_offset=%g out of range (0,100], keeping %g",
                  (double)units, (double)g_amp);
    }
}

void set_entry_inject_hz(float hz)
{
    if (hz > 0.01f && hz <= 5.0f) {
        g_hz = hz;
        MC2VR_LOG("inject: offset frequency = %g Hz", (double)hz);
    } else {
        MC2VR_LOG("inject: debug_entry_inject_hz=%g out of range (0.01,5], keeping %g",
                  (double)hz, (double)g_hz);
    }
}

void on_view(uint32_t idx, uint32_t type, const uint8_t *entry)
{
    if (!g_enabled || entry == nullptr || type != MC2_VIEW_TYPE2) {
        return;
    }

    // Liveness: only t3=01 persistent views carry the live camera.
    const uint8_t t3 = *(const uint8_t *)(MC2_VIEW_TABLE3 +
                                          (uintptr_t)idx * MC2_VIEW_TABLE3_STRIDE +
                                          MC2_VIEW_T3_OFF);
    if (t3 != (uint8_t)MC2_VIEW_T3_LIVE) {
        return;
    }

    // Slot0 = 3x4 at entry+0x20, THREE ROWS OF FOUR dwords (stride 4 — the
    // Matrix_MakeWorldToView transpose proves the layout): rows m0..2/m4..6/
    // m8..10 = camera basis (right/up/fwd), last column m3/m7/m11 = the folded
    // translation. Axis-convention candidates (row layout vs column layout)
    // are both tried; the first 6 live views also dump raw values + dots for
    // one-run ground truth.
    const float *mx = (const float *)(entry + MC2_VIEW_OFF_SLOT0);
    Vec3 row_right, row_fwd, col_right, col_fwd;
    const bool row_ok =
        math::normalize(Vec3{mx[0], mx[1], mx[2]}, &row_right) &&
        math::normalize(Vec3{mx[8], mx[9], mx[10]}, &row_fwd);
    const bool col_ok =
        math::normalize(Vec3{mx[0], mx[4], mx[8]}, &col_right) &&
        math::normalize(Vec3{mx[2], mx[6], mx[10]}, &col_fwd);
    // Slot0 can be garbage/identity (2026-10-06 run 2: IDENTITY on all dumped
    // live views) — its failure must not block the position match below.

    vpcam::Camera cam;
    if (!view::get_game_camera(&cam)) {
        if (g_no_cam_logged++ < 4) {
            MC2VR_LOG("inject: no decomposed game camera yet — matching idle "
                      "(view_row_rewrite must be hmd or hmd_identity)");
        }
        return;
    }

    // PRIMARY match: position. pos7c4 is the proven change-gated camera
    // field and needs no rotation-convention knowledge (2026-10-06 run 2:
    // every dumped live view's slot0 was IDENTITY — basis matching alone
    // cannot identify the view; the entry pos is the reliable signal).
    const Vec3 entry_pos = read_v3(entry + MC2_VIEW_OFF_POS_CUR);
    const float pos_d = math::distance(entry_pos, cam.C);

    // Per-window closest-view diagnostic (steady-state ground truth).
    if (!g_best.valid || pos_d < g_best.d) {
        g_best.valid = true;
        g_best.d = pos_d;
        g_best.idx = idx;
        g_best.pos = entry_pos;
        g_best.live_views++;
    } else {
        g_best.live_views++;
    }
    if (pos_d < POS_TOL) {
        g_best.within_tol++;
    }

    const float row_dot_r = row_ok ? fabsf(math::dot(row_right, cam.R)) : 0.0f;
    const float row_dot_f = row_ok ? fabsf(math::dot(row_fwd, cam.F)) : 0.0f;
    const float col_dot_r = col_ok ? fabsf(math::dot(col_right, cam.R)) : 0.0f;
    const float col_dot_f = col_ok ? fabsf(math::dot(col_fwd, cam.F)) : 0.0f;

    // One-shot layout diagnostic (first live views seen with a camera).
    if (g_dump_n < 24) {
        g_dump_n++;
        const float *q = (const float *)(entry + MC2_VIEW_OFF_QUAT);
        MC2VR_LOG("inject: dump idx=%u pos=[%.2f %.2f %.2f] posd=%.2f "
                  "quat=[%.3f %.3f %.3f %.3f] m=[%.3f %.3f %.3f %.3f | %.3f "
                  "%.3f %.3f %.3f | %.3f %.3f %.3f %.3f] camR=[%.3f %.3f %.3f] "
                  "camF=[%.3f %.3f %.3f] camC=[%.2f %.2f %.2f] dots "
                  "row(R,F)=%.3f,%.3f col=%.3f,%.3f",
                  idx, (double)entry_pos.x, (double)entry_pos.y, (double)entry_pos.z,
                  (double)pos_d,
                  (double)q[0], (double)q[1], (double)q[2], (double)q[3],
                  (double)mx[0], (double)mx[1], (double)mx[2], (double)mx[3],
                  (double)mx[4], (double)mx[5], (double)mx[6], (double)mx[7],
                  (double)mx[8], (double)mx[9], (double)mx[10], (double)mx[11],
                  (double)cam.R.x, (double)cam.R.y, (double)cam.R.z,
                  (double)cam.F.x, (double)cam.F.y, (double)cam.F.z,
                  (double)cam.C.x, (double)cam.C.y, (double)cam.C.z,
                  (double)row_dot_r, (double)row_dot_f, (double)col_dot_r,
                  (double)col_dot_f);
    }

    // Match: position (primary — convention-free; 2026-10-06 run 5 proved it
    // finds the camera views: idx 20-23 at posd 3-7). Basis fallback must be
    // guarded: an IDENTITY slot0 false-positived against an axis-aligned
    // camera that run (|dot|((1,0,0),(-1,0,0))=1.000) and consumed all match
    // slots before the real camera views were visited.
    const bool slot_identity =
        fabsf(mx[0] - 1.0f) < 1e-6f && fabsf(mx[5] - 1.0f) < 1e-6f &&
        fabsf(mx[10] - 1.0f) < 1e-6f &&
        fabsf(mx[1]) + fabsf(mx[2]) + fabsf(mx[3]) + fabsf(mx[4]) +
                fabsf(mx[6]) + fabsf(mx[7]) + fabsf(mx[8]) + fabsf(mx[9]) +
                fabsf(mx[11]) < 1e-6f;
    Vec3 right;
    bool matches = false;
    if (pos_d < POS_TOL) {
        // Offset direction: the view's own right axis if slot0 has one,
        // else the decomposed camera's right (equivalent for a matched view).
        right = row_ok ? row_right : cam.R;
        matches = true;
    } else if (!slot_identity && row_dot_r > DOT_MIN && row_dot_f > DOT_MIN) {
        right = row_right;
        matches = true;
    } else if (!slot_identity && col_dot_r > DOT_MIN && col_dot_f > DOT_MIN) {
        right = col_right;
        matches = true;
    }
    if (!matches) {
        return;
    }

    // Find or register the matched view (re-verified every frame — view
    // indices are NOT stable across sessions, entries are).
    Matched *m = nullptr;
    for (size_t k = 0; k < g_match_n; k++) {
        if (g_match[k].entry == entry) {
            m = &g_match[k];
            break;
        }
    }
    if (m == nullptr) {
        if (g_match_n >= MATCH_MAX) {
            return;
        }
        m = &g_match[g_match_n++];
        m->entry = entry;
        m->idx = idx;
        // Base the oscillation on the entry's OWN pos7c4 (the slot0 folded
        // translation is not the position under the axes-only match).
        const Vec3 cur0 = read_v3(entry + MC2_VIEW_OFF_POS_CUR);
        m->base = cur0;
        m->last_write = cur0;
        m->last_frame = 0;
        m->injects = 0;
        m->refreshes = 0;
        m->logged = false;
    }
    if (!m->logged) {
        m->logged = true;
        MC2VR_LOG("inject: matched view idx=%u entry=%p posd=%.2f — injecting",
                  idx, (const void *)entry, (double)pos_d);
    }

    // Once per frame per view.
    const uint64_t frame = hooks::frame_count();
    if (frame == m->last_frame) {
        return;
    }
    m->last_frame = frame;

    // Adopt external refreshes (VM copy-back / activation rewrote the entry
    // pos under us): anything not equal to our last write is the game's new
    // camera position — rebase the oscillation on it.
    const Vec3 cur = read_v3(entry + MC2_VIEW_OFF_POS_CUR);
    if (math::distance(cur, m->last_write) > 0.001f) {
        m->base = cur;
        m->refreshes++;
        g_refreshes_window++;
    }

    // Inject: pos_prev = base, pos = base + right*A*sin(...), serial += 1.
    const double t = (double)GetTickCount64() * 0.001;
    const float s = (float)sin(2.0 * 3.141592653589793 * (double)g_hz * t);
    const Vec3 off = right * (g_amp * s);
    const Vec3 new_pos = m->base + off;

    // Verdict regression (see g_sxy/g_sxx): cam_now is the previous frame's
    // decomposed draw camera (uploads happen after the walk), base is the
    // game-truth pos — their difference carries last frame's offset iff the
    // draw camera follows.
    vpcam::Camera cam_now;
    if (view::get_game_camera(&cam_now)) {
        const Vec3 v = cam_now.C - m->base;
        g_sxy += (double)math::dot(off, v);
        g_sxx += (double)math::dot(off, off);
    }

    write_v3((uint8_t *)(entry + MC2_VIEW_OFF_POS_PREV), m->base);
    write_v3((uint8_t *)(entry + MC2_VIEW_OFF_POS_CUR), new_pos);
    volatile uint32_t *serial = (volatile uint32_t *)(entry + MC2_VIEW_OFF_SERIAL);
    *serial = *serial + 1;  // open the change-gate (max+1 semantics; +1 is enough)

    m->last_write = new_pos;
    m->injects++;
    g_injects_window++;
    g_offset_last = off;

    // Follow metric (the measurement): decomposed RAW draw camera minus the
    // first matched view's base — should equal ~offset if the draw camera
    // follows our injection.
    vpcam::Camera now;
    if (view::get_game_camera(&now)) {
        const Vec3 base0 = g_match[0].base;
        g_follow_x = now.C.x - base0.x;
        g_follow_y = now.C.y - base0.y;
        g_follow_z = now.C.z - base0.z;
    }
}

void report_window()
{
    if (!g_enabled) {
        return;
    }
    if (g_match_n == 0) {
        if (g_best.valid) {
            MC2VR_LOG("inject: window: no match yet — closest live view idx=%u "
                      "posd=%.2f pos=[%.2f %.2f %.2f] (%u live, %u within %g)",
                      g_best.idx, (double)g_best.d, (double)g_best.pos.x,
                      (double)g_best.pos.y, (double)g_best.pos.z,
                      g_best.live_views, g_best.within_tol, (double)POS_TOL);
        } else {
            MC2VR_LOG("inject: window: no matched view yet (need gameplay + hmd "
                      "rewrite active)");
        }
        g_best = {};
        return;
    }
    char views[64 * MATCH_MAX];
    int n = 0;
    for (size_t k = 0; k < g_match_n && n < (int)sizeof(views) - 60; k++) {
        n += _snprintf(views + n, sizeof(views) - n,
                       " idx%u:inj=%llu:refresh=%llu", g_match[k].idx,
                       (unsigned long long)g_match[k].injects,
                       (unsigned long long)g_match[k].refreshes);
    }
    views[n] = '\0';
    // THE E2 ANSWER, measured: follow == offset (draw camera follows) vs
    // follow ~ 0 while offset swings (entry is culling-only); slope is the
    // phase-immune regression form of the same comparison.
    const double slope = g_sxx > 1e-9 ? g_sxy / g_sxx : 0.0;
    MC2VR_LOG("inject: window: injects=%llu refreshes=%llu offset=[%.2f %.2f %.2f] "
              "FOLLOW(rawCam-base)=[%.2f %.2f %.2f] SLOPE=%.2f |%s",
              (unsigned long long)g_injects_window,
              (unsigned long long)g_refreshes_window,
              (double)g_offset_last.x, (double)g_offset_last.y,
              (double)g_offset_last.z, (double)g_follow_x, (double)g_follow_y,
              (double)g_follow_z, slope, views);
    g_injects_window = 0;
    g_refreshes_window = 0;
    g_sxy = 0;
    g_sxx = 0;
    g_best = {};
}

}  // namespace mc2vr::injectprobe
