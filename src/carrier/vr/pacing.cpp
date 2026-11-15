#include "vr/pacing.hpp"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "vr/eye_replay.hpp"
#include "game/addresses.h"
#include "log.hpp"
#include "qpc.hpp"

namespace mc2vr::pacing {

namespace {

constexpr double kGateTimeoutMs = 50.0;
// Display deltas outside (0, kMaxDeltaSec] (session transitions, Reset stalls)
// are not fed to the game — its own body time stands for that frame.
constexpr double kMaxDeltaSec = 0.1;
// Governor mode 2 quantizes to the smallest multiple of 1/rate STRICTLY above
// its input; an exact 1/60 input would step 2/60. Feed it slightly less.
constexpr float kQuantizeMarginSec = 0.00025f;

uint64_t g_drains_skipped = 0;

// ---- unpaced frames (no lockstep: VR off, menus, session not shown) --------
// The carrier forces D3DPRESENT_INTERVAL_IMMEDIATE, so without the OpenXR
// frame loop nothing caps the game; above 200 fps the governor's 5 ms minimum
// step runs the simulation faster than real time. Cap the frame rate here and
// feed the real frame interval as dt (the game's own value is its FrameTick
// body time, which would exclude the wait).
constexpr int64_t kUnpacedHz = 120;
constexpr int64_t kSpinMarginUs = 1500;  // sleep until this close, then spin
int64_t g_free_prev = 0;                 // QPC at the previous unpaced frame start
HANDLE g_timer = nullptr;                // high-resolution waitable timer (or null)
bool g_timer_tried = false;
uint64_t g_free_frames = 0, g_free_waited = 0, g_free_fed = 0;
double g_free_wait_ms = 0.0, g_free_dt_sum_ms = 0.0;

// ---- render-thread side (pose sample) --------------------------------------
// Display time of the last rendered pose and the display-period estimate,
// written by the render thread, read by the update thread.
std::atomic<int64_t> g_render_t{0};
std::atomic<int64_t> g_period_ns{0};
constexpr int kPeriodWindow = 32;  // fallback period = min of the last N display deltas
int64_t g_deltas[kPeriodWindow] = {};
int g_delta_next = 0;

// ---- update-thread side (simulation step) ----------------------------------
int64_t g_target_prev = 0;  // display time the previous update was advanced to

// window counters
uint64_t g_frames = 0, g_open = 0;  // frames in / outside the frame loop
uint64_t g_dt_fed = 0, g_dt_skipped = 0, g_step_floored = 0;
double g_delta_sum_ms = 0.0, g_delta_min_ms = 0.0, g_delta_max_ms = 0.0;
double g_body_sum_ms = 0.0;  // the game's own (replaced) value, for comparison
double g_simdt_min_ms = 0.0, g_simdt_max_ms = 0.0;  // resulting g_Dt
uint64_t g_simdt_n = 0;

uint32_t governor_mode()
{
    return *(const volatile uint8_t *)MC2_FRAMERATE_MODE_SEL
               ? *(const volatile uint32_t *)MC2_FRAMERATE_MODE_A
               : *(const volatile uint32_t *)MC2_FRAMERATE_MODE_B;
}

void note_rendered(const xr_session::ViewState &st)
{
    const int64_t t = st.display_time;
    const int64_t prev = g_render_t.load(std::memory_order_relaxed);
    const int64_t d = t - prev;
    if (st.display_period > 0 && st.display_period <= (int64_t)(kMaxDeltaSec * 1e9)) {
        // The runtime's own period for this frame (exact across rate changes,
        // e.g. SteamVR dropping to half rate).
        g_period_ns.store(st.display_period, std::memory_order_relaxed);
    } else if (prev != 0 && d > 0 && d <= (int64_t)(kMaxDeltaSec * 1e9)) {
        // Fallback (no period reported): shortest recent display delta.
        g_deltas[g_delta_next] = d;
        g_delta_next = (g_delta_next + 1) % kPeriodWindow;
        int64_t p = 0;
        for (int64_t x : g_deltas) {
            if (x > 0 && (p == 0 || x < p)) p = x;
        }
        g_period_ns.store(p, std::memory_order_relaxed);
    }
    g_render_t.store(t, std::memory_order_release);
}

void wait_until(int64_t due)
{
    if (!g_timer_tried) {
        g_timer_tried = true;
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
        g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_ALL_ACCESS);
        if (!g_timer) {
            g_timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        }
    }
    const int64_t f = qpc::freq();
    const int64_t margin = f * kSpinMarginUs / 1000000;
    const int64_t remain = due - qpc::now();
    if (remain > margin) {
        const int64_t sleep_100ns = (remain - margin) * 10000000 / f;
        LARGE_INTEGER rel;
        rel.QuadPart = -sleep_100ns;  // relative
        if (g_timer && SetWaitableTimer(g_timer, &rel, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(g_timer, INFINITE);
        } else {
            Sleep((DWORD)(sleep_100ns / 10000));
        }
    }
    while (qpc::now() < due) {
        YieldProcessor();
    }
}

// Update thread, a frame outside lockstep: hold to kUnpacedHz, feed the real
// frame interval.
void regulate_unpaced()
{
    const int64_t f = qpc::freq();
    const int64_t prev = g_free_prev;
    int64_t now = qpc::now();
    if (prev != 0) {
        const int64_t due = prev + f / kUnpacedHz;
        if (now < due) {
            wait_until(due);
            g_free_wait_ms += qpc::to_ms(qpc::now() - now);
            g_free_waited++;
            now = qpc::now();
        }
    }
    g_free_prev = now;
    g_free_frames++;
    if (prev == 0) {
        return;  // first unpaced frame: the game's own dt
    }
    const double delta = (double)(now - prev) / (double)f;
    if (!(delta > 0.0 && delta <= kMaxDeltaSec)) {
        return;  // a load / stall: the game's own body time stands
    }
    float in = (float)delta;
    if (governor_mode() == 2) {
        in -= kQuantizeMarginSec;
    }
    *(volatile float *)MC2_G_FRAMEDELTASEC = in;
    g_free_dt_sum_ms += delta * 1000.0;
    g_free_fed++;
}

} // namespace

void on_frame(const xr_session::Frame &xr)
{
    if (!xr.begun || !xr_session::lockstep(xr.views.session_state)) {
        g_open++;
        g_render_t.store(0, std::memory_order_relaxed);  // re-seed on the next session
        return;
    }
    g_frames++;
    note_rendered(xr.views);
}

void frame_start()
{
    // The previous frame's governed step (diagnostic: should track the deltas).
    const double simdt_ms = *(const volatile float *)MC2_G_DT * 1000.0;
    if (simdt_ms > 0.0) {
        g_simdt_min_ms = g_simdt_n == 0 ? simdt_ms : std::fmin(g_simdt_min_ms, simdt_ms);
        g_simdt_max_ms = std::fmax(g_simdt_max_ms, simdt_ms);
        g_simdt_n++;
    }

    // This frame will display one period after the last rendered pose (when on
    // time); advance the simulation to that time. A late frame is caught up by
    // the next one (its target moves on from the actual display time).
    const int64_t last = g_render_t.load(std::memory_order_acquire);
    const int64_t period = g_period_ns.load(std::memory_order_relaxed);
    if (last == 0 || period == 0) {
        g_target_prev = 0;  // not in lockstep (or not seeded yet): capped, real-time dt
        regulate_unpaced();
        return;
    }
    g_free_prev = 0;  // re-seed the cap when lockstep ends
    // Never advance less than one display period: the update can start before
    // the render thread sampled the previous frame's pose, and a rate switch
    // (SteamVR half rate -> full) can make the last prediction too late —
    // both would otherwise give a zero step followed by a double one.
    const int64_t prev = g_target_prev;
    int64_t target = last + period;
    if (prev != 0 && target < prev + period) {
        target = prev + period;
        g_step_floored++;
    }
    g_target_prev = target;
    const double delta = prev != 0 ? (double)(target - prev) * 1e-9 : 0.0;
    if (!(delta > 0.0 && delta <= kMaxDeltaSec)) {
        g_dt_skipped++;
        return;
    }
    float in = (float)delta;
    if (governor_mode() == 2) {
        in -= kQuantizeMarginSec;
    }
    volatile float *raw = (volatile float *)MC2_G_FRAMEDELTASEC;
    g_body_sum_ms += *raw * 1000.0;
    *raw = in;
    const double ms = delta * 1000.0;
    g_delta_min_ms = g_dt_fed == 0 ? ms : std::fmin(g_delta_min_ms, ms);
    g_delta_max_ms = std::fmax(g_delta_max_ms, ms);
    g_delta_sum_ms += ms;
    g_dt_fed++;
}

bool on_drain_check_busy()
{
    if (eye_replay::current_pass() != 2) {
        return false;  // pass 1 / outside the stereo frame
    }
    g_drains_skipped++;
    return true;  // report "done": skip the spin, take the normal exit
}

void report_window()
{
    MC2VR_LOG("pace: frames paced=%llu open=%llu | period est %.2fms", (unsigned long long)g_frames,
              (unsigned long long)g_open, (double)g_period_ns.load() / 1e6);
    MC2VR_LOG("pace: sim dt fed=%llu skipped=%llu floored=%llu step avg=%.2fms min=%.2fms "
              "max=%.2fms (replaced game body time avg=%.2fms) -> g_Dt min=%.2fms max=%.2fms "
              "| governor mode=%u rate=%.1f",
              (unsigned long long)g_dt_fed, (unsigned long long)g_dt_skipped,
              (unsigned long long)g_step_floored,
              g_dt_fed ? g_delta_sum_ms / (double)g_dt_fed : 0.0, g_delta_min_ms,
              g_delta_max_ms, g_dt_fed ? g_body_sum_ms / (double)g_dt_fed : 0.0,
              g_simdt_min_ms, g_simdt_max_ms, governor_mode(),
              (double)*(const volatile float *)MC2_FRAMERATE_RATE);
    MC2VR_LOG("pace: replay-pass GPU drain skipped=%llu", (unsigned long long)g_drains_skipped);
    MC2VR_LOG("pace: unpaced frames=%llu (cap %lld Hz) waited=%llu wait avg=%.2fms "
              "dt fed=%llu avg=%.2fms",
              (unsigned long long)g_free_frames, (long long)kUnpacedHz,
              (unsigned long long)g_free_waited,
              g_free_waited ? g_free_wait_ms / (double)g_free_waited : 0.0,
              (unsigned long long)g_free_fed,
              g_free_fed ? g_free_dt_sum_ms / (double)g_free_fed : 0.0);
    g_free_frames = g_free_waited = g_free_fed = 0;
    g_free_wait_ms = g_free_dt_sum_ms = 0.0;
    g_drains_skipped = 0;
    g_frames = g_open = 0;
    g_dt_fed = g_dt_skipped = g_step_floored = 0;
    g_delta_sum_ms = g_delta_min_ms = g_delta_max_ms = 0.0;
    g_body_sum_ms = 0.0;
    g_simdt_min_ms = g_simdt_max_ms = 0.0;
    g_simdt_n = 0;
}

} // namespace mc2vr::pacing
