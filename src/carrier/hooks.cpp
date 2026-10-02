#include "hooks.hpp"

#include <safetyhook.hpp>

#include <windows.h>

#include "game_addresses.h"
#include "log.hpp"

namespace mc2vr::hooks {

namespace {

using GetD3DDevice_t = void *(*)();

SafetyHookInline g_frame_tick_hook;

// Frame statistics. FrameTick has a single call site in GameShell_Run's loop,
// i.e. these are touched from the main thread only — no atomics needed.
constexpr double REPORT_INTERVAL_SEC = 10.0;

LARGE_INTEGER g_qpc_freq = {};
LARGE_INTEGER g_prev_qpc = {};
LARGE_INTEGER g_window_start = {};

uint64_t g_total_frames = 0;
uint64_t g_window_frames = 0;
double g_window_dt_sum = 0.0;
double g_window_dt_min = 0.0;
double g_window_dt_max = 0.0;

void log_report(double window_sec)
{
    double avg_ms = g_window_dt_sum / (double)g_window_frames;
    MC2VR_LOG("FrameTick: %llu frames / %.1fs | dt avg %.2fms min %.2fms max %.2fms | total %llu",
              (unsigned long long)g_window_frames, window_sec, avg_ms,
              g_window_dt_min, g_window_dt_max, (unsigned long long)g_total_frames);

    g_window_frames = 0;
    g_window_dt_sum = 0.0;
    g_window_dt_min = 0.0;
    g_window_dt_max = 0.0;
}

// void (void) — the original is convention-agnostic (zero args, void return),
// and so is this handler: cdecl and stdcall are identical for () on i386.
void frame_tick_hook()
{
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
    }

    g_prev_qpc = now;
    g_total_frames++;
    g_window_frames++;

    // ---- original game logic first, instrumentation side effects after ----
    g_frame_tick_hook.call<void>();

    // M1 bonus / M2 pre-work: observe the D3D device once it exists. The
    // call runs on the main thread (render threading rules, render_path.md)
    // and stops as soon as the pointer is non-NULL — normal game code calls
    // this thunk constantly, so per-frame calls are routine for it.
    static void *observed_device = nullptr;
    static uint32_t device_tries = 0;
    constexpr uint32_t DEVICE_TRIE_MAX = 1200; // ~20s at 60fps
    if (!observed_device && device_tries < DEVICE_TRIE_MAX) {
        device_tries++;
        void *device = ((GetD3DDevice_t)MC2_GETD3DDEVICE_THUNK)();
        if (device) {
            observed_device = device;
            MC2VR_LOG("FrameTick: observed IDirect3DDevice9* = %p after %u frames",
                      device, device_tries);
        } else if (device_tries == DEVICE_TRIE_MAX) {
            MC2VR_LOG("FrameTick: no IDirect3DDevice9* after %u frames (D3D init late?",
                      device_tries);
        }
    }

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

} // namespace mc2vr::hooks
