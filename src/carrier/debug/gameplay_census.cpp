// Gameplay census — see gameplay_census.hpp.

#include "debug/gameplay_census.hpp"

#include <safetyhook.hpp>

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "debug/watch.hpp"
#include "game/addresses.h"
#include "hooks/frame_tick.hpp"
#include "hooks/site.hpp"
#include "log.hpp"

namespace mc2vr::gameplay_census {

namespace {

bool g_enabled = false;

// ---- frame phases (set by the update thread at the marker entries) ----------

enum Phase : uint32_t {
    kPhaseNone = 0,  // before Player_FrameUpdate this frame
    kPhasePlayer,    // Player_FrameUpdate .. (incl. the command drain)
    kPhasePhysics,   // HumanPhysics_FrameStep ..
    kPhaseAnim,      // AnimSystem_FrameUpdate .. (the 3 animation task queues)
    kPhaseAnimPost,  // AnimSystem_PostUpdate ..
    kPhaseCamera,    // CameraTable_FillLoop .. (until the next frame's player update)
    kPhaseCount
};
const char *const kPhaseNames[kPhaseCount] = {"none", "player", "physics", "anim", "animpost", "camera"};

// (frame << 8) | phase, written only by the update thread.
std::atomic<uint64_t> g_phase{0};
// Markers seen out of order within a frame (window).
std::atomic<uint32_t> g_order_anomalies{0};
std::atomic<uint32_t> g_marker_hits[kPhaseCount] = {};

void mark(Phase p)
{
    const uint64_t frame = hooks::frame_count();
    const uint64_t prev = g_phase.load(std::memory_order_relaxed);
    if ((prev >> 8) == frame && (Phase)(prev & 0xff) > p && p != kPhasePlayer) {
        g_order_anomalies.fetch_add(1, std::memory_order_relaxed);
    }
    g_phase.store((frame << 8) | p, std::memory_order_release);
    g_marker_hits[p].fetch_add(1, std::memory_order_relaxed);
}

// ---- bone-controller callers ---------------------------------------------------

struct CallerStat {
    uint32_t ra;           // return address (game VA)
    uint32_t tid;
    bool update_thread;
    uint64_t calls;        // window
    uint64_t by_phase[kPhaseCount];
    uint64_t stale_frame;  // the phase stamp belonged to an earlier frame
};
constexpr int kMaxCallers = 16;

struct CallerTable {
    const char *what;
    CRITICAL_SECTION cs;
    CallerStat s[kMaxCallers];
    int n;
    uint64_t dropped;
    bool reported_new[kMaxCallers];
};
CallerTable g_onpose{"OnPose"}, g_slot2{"slot2"};

void record_call(CallerTable &t, uint32_t ra)
{
    const uint32_t tid = GetCurrentThreadId();
    const uint64_t ph = g_phase.load(std::memory_order_acquire);
    const uint32_t phase = (uint32_t)(ph & 0xff) < kPhaseCount ? (uint32_t)(ph & 0xff) : kPhaseNone;
    const bool stale = (ph >> 8) != hooks::frame_count();
    EnterCriticalSection(&t.cs);
    int i = 0;
    for (; i < t.n; i++) {
        if (t.s[i].ra == ra && t.s[i].tid == tid) break;
    }
    if (i == t.n) {
        if (t.n == kMaxCallers) {
            t.dropped++;
            LeaveCriticalSection(&t.cs);
            return;
        }
        t.s[i] = CallerStat{};
        t.s[i].ra = ra;
        t.s[i].tid = tid;
        t.s[i].update_thread = tid == hooks::update_thread_id();
        t.n++;
    }
    CallerStat &c = t.s[i];
    c.calls++;
    c.by_phase[phase]++;
    c.stale_frame += stale ? 1 : 0;
    LeaveCriticalSection(&t.cs);
}

void report_callers(CallerTable &t)
{
    EnterCriticalSection(&t.cs);
    uint64_t total = 0;
    for (int i = 0; i < t.n; i++) total += t.s[i].calls;
    MC2VR_LOG("gameplay: %s window calls=%llu callers=%d dropped=%llu", t.what, (unsigned long long)total, t.n,
              (unsigned long long)t.dropped);
    for (int i = 0; i < t.n; i++) {
        CallerStat &c = t.s[i];
        if (c.calls == 0) continue;
        char where[96];
        describe_code_address((void *)(uintptr_t)c.ra, where, sizeof(where));
        char phases[160];
        int k = 0;
        for (int p = 0; p < kPhaseCount && k >= 0 && (size_t)k < sizeof(phases); p++) {
            if (c.by_phase[p]) {
                k += snprintf(phases + k, sizeof(phases) - k, " %s=%llu", kPhaseNames[p],
                              (unsigned long long)c.by_phase[p]);
            }
        }
        MC2VR_LOG("gameplay: %s caller ra=%s (VA %08X) tid=%u%s calls=%llu phase:%s stale-frame=%llu", t.what, where,
                  c.ra, c.tid, c.update_thread ? " (update thread)" : "", (unsigned long long)c.calls, phases,
                  (unsigned long long)c.stale_frame);
        c.calls = c.stale_frame = 0;
        memset(c.by_phase, 0, sizeof(c.by_phase));
    }
    t.dropped = 0;
    LeaveCriticalSection(&t.cs);
}

// ---- class tables (docs/reverse_engineering/camera_classes.md) --------------

struct Named {
    uint32_t vtbl;
    const char *name;
};
const Named kCameraClasses[] = {
    {0x00bce388, "nullcam"}, {0x00bce3d8, "free eye"}, {0x00bce428, "binoculars"}, {0x00bce478, "pda"},
    {0x00bce4c8, "ragdoll"}, {0x00bd2018, "car"},      {0x00bd2068, "helicopter"}, {0x00bd20b8, "human"},
    {0x00bd2110, "script"},  {0x00bd2160, "tank"},     {0x00bd21b8, "turret"},
};
constexpr uint32_t kCamHumanVtbl = 0x00bd20b8;
const Named kModeClasses[] = {
    {0x00bce858, "null"},    {0x00bce648, "pda"},      {0x00bce6f8, "binoculars"}, {0x00bce6a0, "script"},
    {0x00bce750, "onfoot"},  {0x00bce7a8, "vehicleA"}, {0x00bce800, "vehicleB"},   {0x00bce598, "mode598"},
    {0x00bce5f0, "pda-menu"},  // seen live around opening the PDA (2026-11-11)
};

template <size_t N>
const char *name_of(const Named (&table)[N], uint32_t vtbl)
{
    for (const Named &n : table) {
        if (n.vtbl == vtbl) return n.name;
    }
    return nullptr;
}

uint32_t vtbl_of(uintptr_t obj) { return obj ? *(const uint32_t *)obj : 0; }

// ---- camera table (update thread, CameraTable_FillLoop entry) ---------------

uint32_t g_slot_vtbl[MC2_CAMTABLE_SLOTS] = {};

// Human-camera look input, window aggregates.
struct LookStats {
    uint64_t frames;           // frames with a live human camera
    float max_stick, max_in2;  // max |+0x610/+0x614|, |+0x10/+0x14|
    uint64_t stick_frames, in2_frames;
    float yaw_min, yaw_max, pitch_min, pitch_max;
    uint64_t flag7dd_frames;
};
LookStats g_look{};

void look_sample(uintptr_t cam)
{
    const float sx = *(const float *)(cam + 0x610), sy = *(const float *)(cam + 0x614);
    const float ix = *(const float *)(cam + 0x10), iy = *(const float *)(cam + 0x14);
    const float yaw = *(const float *)(cam + 0x618), pitch = *(const float *)(cam + 0x61c);
    const float stick = fmaxf(fabsf(sx), fabsf(sy)), in2 = fmaxf(fabsf(ix), fabsf(iy));
    if (g_look.frames == 0) {
        g_look.yaw_min = g_look.yaw_max = yaw;
        g_look.pitch_min = g_look.pitch_max = pitch;
    }
    g_look.frames++;
    g_look.max_stick = fmaxf(g_look.max_stick, stick);
    g_look.max_in2 = fmaxf(g_look.max_in2, in2);
    g_look.stick_frames += stick > 0.01f ? 1 : 0;
    g_look.in2_frames += in2 > 1e-5f ? 1 : 0;
    g_look.yaw_min = fminf(g_look.yaw_min, yaw);
    g_look.yaw_max = fmaxf(g_look.yaw_max, yaw);
    g_look.pitch_min = fminf(g_look.pitch_min, pitch);
    g_look.pitch_max = fmaxf(g_look.pitch_max, pitch);
    g_look.flag7dd_frames += *(const uint8_t *)(cam + 0x7dd) ? 1 : 0;
}

void on_camera_table()
{
    for (uintptr_t i = 0; i < MC2_CAMTABLE_SLOTS; i++) {
        const uintptr_t slot = MC2_G_CAMTABLE + i * MC2_CAMTABLE_SLOT_STRIDE;
        const uintptr_t cam = *(const uintptr_t *)(slot + MC2_CAMTABLE_SLOT_CAMERA_OFF);
        const uint32_t vt = vtbl_of(cam);
        if (vt != g_slot_vtbl[i]) {
            const char *was = name_of(kCameraClasses, g_slot_vtbl[i]);
            const char *now = name_of(kCameraClasses, vt);
            MC2VR_LOG("gameplay: camtable slot %u camera %s(%08X) -> %s(%08X) obj=%p frame %llu", (unsigned)i,
                      was ? was : (g_slot_vtbl[i] ? "?" : "-"), g_slot_vtbl[i], now ? now : (vt ? "?" : "-"), vt,
                      (void *)cam, (unsigned long long)hooks::frame_count());
            g_slot_vtbl[i] = vt;
        }
        if (vt == kCamHumanVtbl) {
            look_sample(cam);
        }
    }
}

// ---- player (update thread, Player_FrameUpdate entry, EAX = player) ---------

// The game updates every player slot each frame (also the unused co-op slot,
// which has no character: Player_FrameUpdate returns early on +0x20 == 0).
// Only players with a character are tracked; the first one is the primary
// (handed to the watch tool). Mode stacks are compared per player.
constexpr uintptr_t kPlayerCharacterOff = 0x20;
struct PlayerState {
    uintptr_t obj;
    uint32_t mode_vtbl[8];
    int mode_count;
    uint8_t mode_mask;
};
PlayerState g_players[2] = {};
uintptr_t g_primary = 0;

void on_player(uintptr_t player)
{
    if (player == 0 || *(const uint32_t *)(player + kPlayerCharacterOff) == 0) return;
    PlayerState *ps = nullptr;
    for (PlayerState &p : g_players) {
        if (p.obj == player) { ps = &p; break; }
    }
    if (ps == nullptr) {
        for (PlayerState &p : g_players) {
            if (p.obj == 0) { ps = &p; break; }
        }
        if (ps == nullptr) return;  // more than two players with characters: ignore the rest
        *ps = PlayerState{};
        ps->obj = player;
        ps->mode_count = -2;
        MC2VR_LOG("gameplay: player object %p with character %08X frame %llu%s", (void *)player,
                  *(const uint32_t *)(player + kPlayerCharacterOff), (unsigned long long)hooks::frame_count(),
                  g_primary == 0 ? " (primary)" : "");
        if (g_primary == 0) {
            g_primary = player;
            watch::provide_base(player);
        }
    }
    int count = *(const int *)(player + MC2_PLAYER_MODE_COUNT_OFF);
    if (count < 0 || count > 8) count = -1;
    const uint8_t mask = *(const uint8_t *)(player + MC2_PLAYER_MODE_MASK_OFF);
    uint32_t vt[8] = {};
    for (int i = 0; i < count; i++) {
        vt[i] = vtbl_of(*(const uintptr_t *)(player + MC2_PLAYER_MODES_OFF + 4 * i));
    }
    if (count == ps->mode_count && mask == ps->mode_mask && memcmp(vt, ps->mode_vtbl, sizeof(vt)) == 0) return;
    char line[400];
    int k = snprintf(line, sizeof(line), "gameplay: player %p overlay modes count=%d mask=%02X:", (void *)player,
                     count, mask);
    for (int i = 0; i < count && k > 0 && (size_t)k < sizeof(line) - 40; i++) {
        const char *n = name_of(kModeClasses, vt[i]);
        k += snprintf(line + k, sizeof(line) - k, " [%d]%s%s(%08X)", i, (mask >> i) & 1 ? "*" : "", n ? n : "?",
                      vt[i]);
    }
    MC2VR_LOG("%s frame %llu", line, (unsigned long long)hooks::frame_count());
    ps->mode_count = count;
    ps->mode_mask = mask;
    memcpy(ps->mode_vtbl, vt, sizeof(vt));
}

// ---- hooks ----------------------------------------------------------------

SafetyHookMid g_mid_player, g_mid_physics, g_mid_anim, g_mid_animpost, g_mid_camera, g_mid_onpose, g_mid_slot2;

bool on_update_thread() { return GetCurrentThreadId() == hooks::update_thread_id(); }

void player_mid(safetyhook::Context &ctx)
{
    if (on_update_thread()) mark(kPhasePlayer);
    on_player(ctx.eax);
}
void physics_mid(safetyhook::Context &) { if (on_update_thread()) mark(kPhasePhysics); }
void anim_mid(safetyhook::Context &) { if (on_update_thread()) mark(kPhaseAnim); }
void animpost_mid(safetyhook::Context &) { if (on_update_thread()) mark(kPhaseAnimPost); }
void camera_mid(safetyhook::Context &)
{
    if (on_update_thread()) mark(kPhaseCamera);
    on_camera_table();
}
void onpose_mid(safetyhook::Context &ctx) { record_call(g_onpose, *(const uint32_t *)ctx.esp); }
void slot2_mid(safetyhook::Context &ctx) { record_call(g_slot2, *(const uint32_t *)ctx.esp); }

} // namespace

bool set_enabled(const char *value)
{
    if (strcmp(value, "on") == 0) g_enabled = true;
    else if (strcmp(value, "off") == 0) g_enabled = false;
    else return false;
    MC2VR_LOG("gameplay: debug_gameplay_census=%s", value);
    return true;
}

void install()
{
    if (!g_enabled) return;
    InitializeCriticalSection(&g_onpose.cs);
    InitializeCriticalSection(&g_slot2.cs);
    struct Site {
        SafetyHookMid *hook;
        uintptr_t va;
        safetyhook::MidHookFn fn;
        const char *what;
    };
    const Site sites[] = {
        {&g_mid_player, MC2_PLAYER_FRAME_UPDATE, player_mid, "gameplay: Player_FrameUpdate entry"},
        {&g_mid_physics, MC2_HUMAN_PHYSICS_FRAME_STEP, physics_mid, "gameplay: HumanPhysics_FrameStep entry"},
        {&g_mid_anim, MC2_ANIM_FRAME_UPDATE, anim_mid, "gameplay: AnimSystem_FrameUpdate entry"},
        {&g_mid_animpost, MC2_ANIM_POST_UPDATE, animpost_mid, "gameplay: AnimSystem_PostUpdate entry"},
        {&g_mid_camera, MC2_CAMTABLE_FILL_LOOP, camera_mid, "gameplay: CameraTable_FillLoop entry"},
        {&g_mid_onpose, MC2_BODYBONES_ON_POSE, onpose_mid, "gameplay: BodyBoneTracker_OnPose entry"},
        {&g_mid_slot2, MC2_BODYBONES_SLOT2, slot2_mid, "gameplay: BodyBoneTracker slot-2 entry"},
    };
    int ok = 0;
    for (const Site &s : sites) {
        ok += hooks::create_mid(*s.hook, s.va, s.fn, s.what) ? 1 : 0;
    }
    MC2VR_LOG("gameplay: census installed %d/%d MidHooks", ok, (int)(sizeof(sites) / sizeof(sites[0])));
}

void report_window()
{
    if (!g_enabled) return;
    char markers[200];
    int k = 0;
    for (int p = 1; p < kPhaseCount && k >= 0 && (size_t)k < sizeof(markers); p++) {
        k += snprintf(markers + k, sizeof(markers) - k, " %s=%u", kPhaseNames[p], g_marker_hits[p].exchange(0));
    }
    MC2VR_LOG("gameplay: window markers:%s order-anomalies=%u", markers, g_order_anomalies.exchange(0));
    report_callers(g_onpose);
    report_callers(g_slot2);
    if (g_look.frames) {
        MC2VR_LOG("gameplay: human cam frames=%llu stick(+610/614) max=%.3f frames=%llu | in2(+10/14) max=%.4f "
                  "frames=%llu | yaw [%.3f, %.3f] pitch [%.3f, %.3f] | +7dd set frames=%llu",
                  (unsigned long long)g_look.frames, g_look.max_stick, (unsigned long long)g_look.stick_frames,
                  g_look.max_in2, (unsigned long long)g_look.in2_frames, g_look.yaw_min, g_look.yaw_max,
                  g_look.pitch_min, g_look.pitch_max, (unsigned long long)g_look.flag7dd_frames);
    } else {
        MC2VR_LOG("gameplay: human cam not live this window");
    }
    g_look = LookStats{};
}

} // namespace mc2vr::gameplay_census
