#include "hooks/frame_tick.hpp"

#include <safetyhook.hpp>

#include <windows.h>

#include <cmath>
#include <cstdint>

#include "debug/wait_census.hpp"
#include "game/addresses.h"
#include "hooks/site.hpp"
#include "ipc.hpp"
#include "log.hpp"
#include "vr/hud.hpp"
#include "vr/pacing.hpp"

namespace mc2vr::hooks {

namespace {

SafetyHookInline g_frame_tick_hook;

// Frame statistics. FrameTick has a single call site in GameShell_Run's loop,
// i.e. these are touched from the main thread only — no atomics needed.
constexpr double REPORT_INTERVAL_SEC = 10.0;

LARGE_INTEGER g_qpc_freq = {};
LARGE_INTEGER g_prev_qpc = {};
LARGE_INTEGER g_window_start = {};

uint64_t g_total_frames = 0;
volatile DWORD g_update_tid = 0;  // thread running GameShell_FrameTick
uint64_t g_window_frames = 0;
double g_window_dt_sum = 0.0;
double g_window_dt_min = 0.0;
double g_window_dt_max = 0.0;
double g_window_jit_sum = 0.0;  // sum |dt - previous dt| (frame-to-frame jitter)
double g_prev_dt_ms = 0.0;

void log_report(double window_sec)
{
    double avg_ms = g_window_dt_sum / (double)g_window_frames;
    MC2VR_LOG("FrameTick: %llu frames / %.1fs | dt avg %.2fms min %.2fms max %.2fms "
              "jitter %.2fms (mean |dt - prev dt|) | total %llu",
              (unsigned long long)g_window_frames, window_sec, avg_ms,
              g_window_dt_min, g_window_dt_max,
              g_window_jit_sum / (double)g_window_frames, (unsigned long long)g_total_frames);
    g_window_jit_sum = 0.0;

    g_window_frames = 0;
    g_window_dt_sum = 0.0;
    g_window_dt_min = 0.0;
    g_window_dt_max = 0.0;
}

// void (void) — the original is convention-agnostic (zero args, void return),
// and so is this handler: cdecl and stdcall are identical for () on i386.
void frame_tick_hook()
{
    // Lockstep pacing BEFORE the game's own FrameTick runs: the display-time
    // step fed to the dt governor (docs/pacing.md; the gate itself is on the
    // render thread's pose sample).
    g_update_tid = GetCurrentThreadId();
    wait_census::mark(wait_census::Mark::FrameEntry);
    pacing::frame_start();
    wait_census::mark(wait_census::Mark::GateDone);

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    const double dt_ms =
        g_prev_qpc.QuadPart != 0
            ? (double)(now.QuadPart - g_prev_qpc.QuadPart) * 1000.0 / (double)g_qpc_freq.QuadPart
            : 0.0;

    if (g_total_frames == 0) {
        // First entry after install: hook-activation ack. The game's own
        // FrameTick opens with the same QPC sample, so timing is unaffected.
        MC2VR_LOG("FrameTick: first frame entered (hook active)");
        g_window_start = now;
    } else {
        g_window_dt_sum += dt_ms;
        if (g_window_dt_min == 0.0 || dt_ms < g_window_dt_min) {
            g_window_dt_min = dt_ms;
        }
        if (dt_ms > g_window_dt_max) {
            g_window_dt_max = dt_ms;
        }
        if (g_prev_dt_ms > 0.0) {
            g_window_jit_sum += std::fabs(dt_ms - g_prev_dt_ms);
        }
        g_prev_dt_ms = dt_ms;
    }

    g_prev_qpc = now;
    g_total_frames++;
    g_window_frames++;

    // ---- original game logic first, instrumentation side effects after ----
    g_frame_tick_hook.call<void>();
    wait_census::mark(wait_census::Mark::FrameExit);

    if (g_window_start.QuadPart != 0) {
        double window_sec =
            (double)(now.QuadPart - g_window_start.QuadPart) / (double)g_qpc_freq.QuadPart;
        if (window_sec >= REPORT_INTERVAL_SEC) {
            log_report(window_sec);
            g_window_start = now;
        }
    }
}

} // namespace

bool install()
{
    if (g_frame_tick_hook) {
        return true; // already installed
    }

    // QPC frequency before the hook can possibly fire (create() activates
    // the jmp as its last step — a frame can tick between activation and
    // the assignments below).
    if (!QueryPerformanceFrequency(&g_qpc_freq) || g_qpc_freq.QuadPart == 0) {
        MC2VR_LOG("FATAL: QueryPerformanceFrequency failed — cannot install FrameTick hook");
        return false;
    }

    // Install WITHOUT suspending other threads: SafetyHook v0.7.0's install
    // is trap-based (target page made non-executable during patching, a
    // vectored-exception handler redirects any thread whose IP lands in the
    // patched range), so install is already atomic w.r.t. execution.
    // Externally suspending threads first would instead risk deadlock:
    // v0.7.0 heap-allocates during create_inline, and a suspended thread
    // holding the CRT heap lock would block that allocation forever.
    // (Supersedes the plan's original SuspendThread-all-but-self discipline.)
    auto result = SafetyHookInline::create(reinterpret_cast<uint8_t *>(MC2_GAMESHELL_FRAMETICK),
                                           reinterpret_cast<uint8_t *>(frame_tick_hook));
    if (!result) {
        MC2VR_LOG("FATAL: FrameTick hook install failed @ %p (error %u)",
                  (void *)MC2_GAMESHELL_FRAMETICK, (unsigned)result.error().type);
        return false;
    }

    g_frame_tick_hook = std::move(*result);
    MC2VR_LOG("hooks: installed FrameTick @ %p (trap-based install, no external suspension)",
              (void *)MC2_GAMESHELL_FRAMETICK);

    return true;
}

uint64_t frame_count()
{
    return g_total_frames;
}

uint32_t update_thread_id()
{
    return g_update_tid;
}

namespace {

SafetyHookMid g_post_update_mid;
uint64_t g_post_update_calls = 0;

void post_update_midhook(safetyhook::Context &)
{
    if (g_post_update_calls++ == 0) {
        MC2VR_LOG("PostUpdateHook call site (g_RenderShell slot 5) hit, frame=%llu",
                  (unsigned long long)frame_count());
    }
    // The host-event drain point (main thread, once per frame — before the
    // next pass-1 pose sample so events apply to THIS frame's camera).
    // No-op when disconnected. Pose sampling itself happens at pass-1 start.
    ipc::drain_events();
    hud::on_update_tick();  // HUD layout mode switch (update thread)
}

} // namespace

void install_post_update()
{
    if (create_mid(g_post_update_mid, MC2_FRAMETICK_POSTUPDATE_SITE, post_update_midhook,
                   "FrameTick PostUpdateHook call site")) {
        MC2VR_LOG("installed FrameTick PostUpdateHook call site MidHook @ %p", (void *)MC2_FRAMETICK_POSTUPDATE_SITE);
    } else {
        MC2VR_LOG("FATAL — no host-event drain: HMD session events are not applied");
    }
}

uint64_t post_update_calls() { return g_post_update_calls; }

} // namespace mc2vr::hooks
