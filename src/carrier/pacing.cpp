#include "pacing.hpp"

#include <safetyhook.hpp>

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#include "eye_replay.hpp"
#include "game_addresses.h"
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

SafetyHookMid g_drain_mid;
bool g_keep_replay_drain = false;
uint64_t g_drains_skipped = 0, g_drains_kept = 0;

// At BeginSubmit's drain check: EAX = first GetData result (1 = GPU busy).
void drain_check_midhook(safetyhook::Context &ctx)
{
    if (eye_replay::current_pass() != 2 || ctx.eax != 1) {
        return;  // pass 1 / outside the stereo frame, or the GPU is already idle
    }
    if (g_keep_replay_drain) {
        g_drains_kept++;
        return;
    }
    ctx.eax = 0;  // report "done": skip the spin, take the normal exit
    g_drains_skipped++;
}

uint32_t g_last_id = 0;      // hostFrame+1 of the last pass-1 sample
int64_t g_prev_display = 0;  // displayTime (ns) of the previous frame's pose

// window counters
uint64_t g_waits = 0, g_timeouts = 0, g_open = 0;
double g_wait_sum_ms = 0.0, g_wait_max_ms = 0.0;
uint64_t g_dt_fed = 0, g_dt_skipped = 0;
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

// Wait for a publish newer than the last rendered pose. False = no lockstep
// this frame (no host / not VISIBLE / timed out); *st = the latest state.
bool gate(Mc2IpcState *st)
{
    if (!read_state_retry(st)) {
        return false;
    }
    if (!lockstep_state(st->sessionState)) {
        g_open++;
        return false;
    }
    if (st->hostFrame + 1 != g_last_id) {
        return true;  // already published past the last rendered pose
    }
    const double t0 = qpc_ms();
    bool fresh = false;
    for (;;) {
        Sleep(0);
        if (!read_state_retry(st)) {
            break;
        }
        if (!lockstep_state(st->sessionState)) {
            break;
        }
        if (st->hostFrame + 1 != g_last_id) {
            fresh = true;
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
    return fresh;
}

} // namespace

void frame_start()
{
    // The previous frame's governed step (diagnostic: should track the deltas).
    const double simdt_ms = *(const volatile float *)MC2_G_DT * 1000.0;
    if (simdt_ms > 0.0) {
        g_simdt_min_ms = g_simdt_n == 0 ? simdt_ms : std::fmin(g_simdt_min_ms, simdt_ms);
        g_simdt_max_ms = std::fmax(g_simdt_max_ms, simdt_ms);
        g_simdt_n++;
    }

    Mc2IpcState st;
    if (!gate(&st)) {
        g_prev_display = 0;  // re-seed after any non-lockstep frame
        return;
    }
    const int64_t t = st.displayTime;
    const double delta = g_prev_display != 0 ? (double)(t - g_prev_display) * 1e-9 : 0.0;
    g_prev_display = t;
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

void on_pose_sampled(uint32_t pose_id)
{
    g_last_id = pose_id;
}

void install()
{
    auto mid = SafetyHookMid::create(reinterpret_cast<uint8_t *>(MC2_LTI_BEGINSUBMIT_DRAIN_CHECK),
                                     drain_check_midhook);
    if (!mid) {
        MC2VR_LOG("pace: BeginSubmit drain MidHook install failed (error %u) — replay pass "
                  "keeps the engine's GPU drain",
                  (unsigned)mid.error().type);
        return;
    }
    g_drain_mid = std::move(*mid);
    MC2VR_LOG("pace: BeginSubmit drain check hooked @ %p (replay pass %s the GPU drain)",
              (void *)MC2_LTI_BEGINSUBMIT_DRAIN_CHECK, g_keep_replay_drain ? "keeps" : "skips");
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
    MC2VR_LOG("pace: gate waits=%llu avg=%.2fms max=%.2fms timeouts=%llu open=%llu | state "
              "read retries=%llu fails=%llu (open = outside a VISIBLE/FOCUSED session)",
              (unsigned long long)g_waits, g_waits ? g_wait_sum_ms / (double)g_waits : 0.0,
              g_wait_max_ms, (unsigned long long)g_timeouts, (unsigned long long)g_open,
              (unsigned long long)g_read_retries, (unsigned long long)g_read_fails);
    g_read_retries = g_read_fails = 0;
    MC2VR_LOG("pace: sim dt fed=%llu skipped=%llu display delta avg=%.2fms min=%.2fms "
              "max=%.2fms (replaced game body time avg=%.2fms) -> g_Dt min=%.2fms max=%.2fms "
              "| governor mode=%u rate=%.1f",
              (unsigned long long)g_dt_fed, (unsigned long long)g_dt_skipped,
              g_dt_fed ? g_delta_sum_ms / (double)g_dt_fed : 0.0, g_delta_min_ms,
              g_delta_max_ms, g_dt_fed ? g_body_sum_ms / (double)g_dt_fed : 0.0,
              g_simdt_min_ms, g_simdt_max_ms, governor_mode(),
              (double)*(const volatile float *)MC2_FRAMERATE_RATE);
    MC2VR_LOG("pace: replay-pass GPU drain skipped=%llu kept=%llu",
              (unsigned long long)g_drains_skipped, (unsigned long long)g_drains_kept);
    g_drains_skipped = g_drains_kept = 0;
    g_waits = g_timeouts = g_open = 0;
    g_wait_sum_ms = g_wait_max_ms = 0.0;
    g_dt_fed = g_dt_skipped = 0;
    g_delta_sum_ms = g_delta_min_ms = g_delta_max_ms = 0.0;
    g_body_sum_ms = 0.0;
    g_simdt_min_ms = g_simdt_max_ms = 0.0;
    g_simdt_n = 0;
}

} // namespace mc2vr::pacing
