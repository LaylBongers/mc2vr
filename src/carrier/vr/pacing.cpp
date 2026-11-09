#include "vr/pacing.hpp"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "vr/eye_replay.hpp"
#include "game/addresses.h"
#include "ipc.hpp"
#include "log.hpp"

namespace mc2vr::pacing {

namespace {

constexpr double kGateTimeoutMs = 50.0;
// Display deltas outside (0, kMaxDeltaSec] (session transitions, Reset stalls)
// are not fed to the game — its own body time stands for that frame.
constexpr double kMaxDeltaSec = 0.1;
// Governor mode 2 quantizes to the smallest multiple of 1/rate STRICTLY above
// its input; an exact 1/60 input would step 2/60. Feed it slightly less.
constexpr float kQuantizeMarginSec = 0.00025f;

bool g_keep_replay_drain = false;
uint64_t g_drains_skipped = 0, g_drains_kept = 0;

// ---- render-thread side (pose sample) --------------------------------------
uint32_t g_last_id = 0;  // hostFrame+1 of the last pass-1 sample
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
uint64_t g_waits = 0, g_timeouts = 0, g_open = 0;
double g_wait_sum_ms = 0.0, g_wait_max_ms = 0.0;
uint64_t g_dt_fed = 0, g_dt_skipped = 0, g_step_floored = 0;
double g_delta_sum_ms = 0.0, g_delta_min_ms = 0.0, g_delta_max_ms = 0.0;
double g_body_sum_ms = 0.0;  // the game's own (replaced) value, for comparison
double g_simdt_min_ms = 0.0, g_simdt_max_ms = 0.0;  // resulting g_Dt
uint64_t g_simdt_n = 0;

double qpc_ms()
{
    static LARGE_INTEGER f = {};
    if (f.QuadPart == 0) {
        QueryPerformanceFrequency(&f);
    }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

bool lockstep_state(uint32_t session_state)
{
    return session_state == MC2VR_XR_SESSION_VISIBLE ||
           session_state == MC2VR_XR_SESSION_FOCUSED;
}

uint32_t governor_mode()
{
    return *(const volatile uint8_t *)MC2_FRAMERATE_MODE_SEL
               ? *(const volatile uint32_t *)MC2_FRAMERATE_MODE_A
               : *(const volatile uint32_t *)MC2_FRAMERATE_MODE_B;
}

uint64_t g_read_retries = 0, g_read_fails = 0;

// Seqlock read that rides out the host's write window (a single read gives up
// after a few spins while the host is mid-publish, which is routine at the
// gate's polling rate). False only when not connected or the host is stuck.
bool read_state_retry(Mc2IpcState *st)
{
    if (!ipc::connected()) {
        return false;
    }
    for (int i = 0; i < 256; i++) {
        if (ipc::read_state(st)) {
            return true;
        }
        g_read_retries++;
        Sleep(0);
    }
    g_read_fails++;
    return false;
}

void note_rendered(const Mc2IpcState &st)
{
    const int64_t t = st.displayTime;
    const int64_t prev = g_render_t.load(std::memory_order_relaxed);
    const int64_t d = t - prev;
    if (st.displayPeriod > 0 && st.displayPeriod <= (int64_t)(kMaxDeltaSec * 1e9)) {
        // The runtime's own period for this frame (exact across rate changes,
        // e.g. SteamVR dropping to half rate).
        g_period_ns.store(st.displayPeriod, std::memory_order_relaxed);
    } else if (prev != 0 && d > 0 && d <= (int64_t)(kMaxDeltaSec * 1e9)) {
        // Fallback (host without a period): shortest recent display delta.
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

} // namespace

bool render_gate(Mc2IpcState *st)
{
    if (!read_state_retry(st)) {
        return false;
    }
    if (!lockstep_state(st->sessionState)) {
        g_open++;
        g_render_t.store(0, std::memory_order_relaxed);  // re-seed on the next session
        return true;
    }
    if (st->hostFrame + 1 == g_last_id) {
        const double t0 = qpc_ms();
        for (;;) {
            Sleep(0);
            Mc2IpcState n;
            if (!read_state_retry(&n)) {
                break;
            }
            *st = n;
            if (!lockstep_state(st->sessionState) || st->hostFrame + 1 != g_last_id) {
                break;
            }
            if (qpc_ms() - t0 > kGateTimeoutMs) {
                g_timeouts++;
                break;
            }
        }
        const double w = qpc_ms() - t0;
        g_waits++;
        g_wait_sum_ms += w;
        g_wait_max_ms = std::fmax(g_wait_max_ms, w);
    }
    g_last_id = st->hostFrame + 1;
    if (lockstep_state(st->sessionState)) {
        note_rendered(*st);
    }
    return true;
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
        g_target_prev = 0;  // not in lockstep (or not seeded yet): the game's own dt
        return;
    }
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
    if (g_keep_replay_drain) {
        g_drains_kept++;
        return false;
    }
    g_drains_skipped++;
    return true;  // report "done": skip the spin, take the normal exit
}

bool keep_replay_drain()
{
    return g_keep_replay_drain;
}

bool set_keep_replay_drain(const char *value)
{
    if (strcmp(value, "on") == 0) g_keep_replay_drain = true;
    else if (strcmp(value, "off") == 0) g_keep_replay_drain = false;
    else return false;
    MC2VR_LOG("pace: debug_keep_replay_drain=%s", value);
    return true;
}

void report_window()
{
    MC2VR_LOG("pace: render gate waits=%llu avg=%.2fms max=%.2fms timeouts=%llu open=%llu | "
              "state read retries=%llu fails=%llu | period est %.2fms",
              (unsigned long long)g_waits, g_waits ? g_wait_sum_ms / (double)g_waits : 0.0,
              g_wait_max_ms, (unsigned long long)g_timeouts, (unsigned long long)g_open,
              (unsigned long long)g_read_retries, (unsigned long long)g_read_fails,
              (double)g_period_ns.load() / 1e6);
    g_read_retries = g_read_fails = 0;
    MC2VR_LOG("pace: sim dt fed=%llu skipped=%llu floored=%llu step avg=%.2fms min=%.2fms "
              "max=%.2fms (replaced game body time avg=%.2fms) -> g_Dt min=%.2fms max=%.2fms "
              "| governor mode=%u rate=%.1f",
              (unsigned long long)g_dt_fed, (unsigned long long)g_dt_skipped,
              (unsigned long long)g_step_floored,
              g_dt_fed ? g_delta_sum_ms / (double)g_dt_fed : 0.0, g_delta_min_ms,
              g_delta_max_ms, g_dt_fed ? g_body_sum_ms / (double)g_dt_fed : 0.0,
              g_simdt_min_ms, g_simdt_max_ms, governor_mode(),
              (double)*(const volatile float *)MC2_FRAMERATE_RATE);
    MC2VR_LOG("pace: replay-pass GPU drain skipped=%llu kept=%llu",
              (unsigned long long)g_drains_skipped, (unsigned long long)g_drains_kept);
    g_drains_skipped = g_drains_kept = 0;
    g_waits = g_timeouts = g_open = 0;
    g_wait_sum_ms = g_wait_max_ms = 0.0;
    g_dt_fed = g_dt_skipped = g_step_floored = 0;
    g_delta_sum_ms = g_delta_min_ms = g_delta_max_ms = 0.0;
    g_body_sum_ms = 0.0;
    g_simdt_min_ms = g_simdt_max_ms = 0.0;
    g_simdt_n = 0;
}

} // namespace mc2vr::pacing
