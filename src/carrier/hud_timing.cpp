#include "hud_timing.hpp"

#include <windows.h>

#include "game_addresses.h"
#include "log.hpp"

namespace mc2vr::hud_timing {

namespace {

// PROTOCOL (runtime-derived 2026-10-06 from the raw burst, menu + gameplay,
// docs/reverse_engineering/view_table.md — supersedes the earlier
// packed-halves model, which is contradicted at runtime):
//   +0x10 high16 = pending-unconsumed element count (producers += during the
//                  frame; the VM consumer clears it, advancing low16, between
//                  SubmitToGPU entry and BeginSubmit's Present)
//   +0x10 low16   = ring position (moves on publish/consume; FROZEN during
//                  both pass walks and the inter-pass gap)
//   +0x14         = never moves at runtime (unused by the live path)
// HUD conclusion: both queues are fully consumed BEFORE pass 1 begins
// drawing, and both passes walk the same record table (frame replay) — so 2D/HUD
// content is drawn into both eyes' composites. One-eye HUD is impossible;
// no host quad layer needed.
//
// What remains here: a one-shot raw diagnostic burst (first 3 frames after
// the first 10s window — game live by then) for any future queue-protocol
// question. Read-only, ~15 log lines per run.

enum Last { L_NONE, L_B1, L_B2, L_B0, L_P };

Last g_last = L_NONE;
bool g_inited = false;
bool g_enabled = true;
bool g_first_window_done = false;
bool g_burst_done = false;
uint32_t g_win_frames = 0;
constexpr uint32_t BURST_FRAMES = 3;
ULONGLONG g_window_start = 0;

void init_once()
{
    g_inited = true;
    g_window_start = GetTickCount64();

    const uint32_t elem = *(volatile uint32_t *)MC2_QUEUE2_ELEM_SIZE;
    const uint32_t cap = *(volatile uint32_t *)MC2_QUEUE2_CAPACITY;
    const uintptr_t buf = *(volatile uintptr_t *)MC2_QUEUE2_BUFFER;

    MC2VR_LOG("hud2: queue2 elem=%u cap=%u buffer=%p",
              elem, cap, (void *)buf);

    if (elem == 0 || cap == 0 || (cap & (cap - 1)) != 0 || buf == 0) {
        MC2VR_LOG("hud2: queue2 struct implausible — diagnostic disabled");
        g_enabled = false;
    }
}

void sample(bool is_present, uint32_t boundary_pass)
{
    if (!g_enabled) return;
    if (!g_inited) init_once();
    if (!g_enabled) return;

    if (!is_present && boundary_pass == 1) {
        g_win_frames++;
    }

    // Burst window: first 3 frames after the first 10s rollover (the game is
    // live by then; frames before it are boot zeros).
    if (g_burst_done || !g_first_window_done || g_win_frames > BURST_FRAMES) {
        // fall through to state update only
    } else {
        const char *point;
        if (is_present) {
            point = (g_last == L_B1) ? "p1" : (g_last == L_B2) ? "p2" : "pX";
        } else {
            point = (boundary_pass == 1) ? "b1"
                    : (boundary_pass == 2) ? "b2" : "b0";
        }
        MC2VR_LOG("hud2 raw f%u %s: q1A=%08x q1B=%08x q2A=%08x q2B=%08x",
                  g_win_frames, point,
                  *(volatile uint32_t *)MC2_QUEUE_COUNTERS_A,
                  *(volatile uint32_t *)MC2_QUEUE_COUNTERS_B,
                  *(volatile uint32_t *)MC2_QUEUE2_COUNTERS_A,
                  *(volatile uint32_t *)MC2_QUEUE2_COUNTERS_B);
        if (g_win_frames == BURST_FRAMES && boundary_pass == 0 && !is_present) {
            g_burst_done = true; // last point of the last burst frame
        }
    }

    g_last = is_present ? L_P
            : (boundary_pass == 1 ? L_B1
               : (boundary_pass == 2 ? L_B2 : L_B0));

    const ULONGLONG now = GetTickCount64();
    if (now - g_window_start >= 10000) {
        g_window_start = now;
        g_first_window_done = true;
        g_win_frames = 0;
    }
}

}  // namespace

void on_present()
{
    sample(true, 0);
}

void on_boundary(uint32_t pass)
{
    sample(false, pass);
}

}  // namespace mc2vr::hud_timing
