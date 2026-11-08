// Lockstep frame pacing with the host (docs/pacing.md).
//   - render_gate(): render thread, at the pass-1 pose sample. While the host
//     session is VISIBLE/FOCUSED, waits (bounded 50 ms) until the host has
//     published a pose newer than the one the last frame rendered with, and
//     returns that state. The update thread is never held, so the next
//     frame's update and render prep overlap the host's wait.
//   - frame_start(): update thread, GameShell_FrameTick entry, before the
//     game's QPC sample: feeds the dt governor the display-time step (last
//     rendered display time + one display period, minus the previous target).
#pragma once

#include <cstdint>

#include "mc2vr_ipc.h"

namespace mc2vr::pacing {

void frame_start();
bool render_gate(Mc2IpcState *st);

// The replay pass (pass 2) skips LtiRenderer_BeginSubmit's GPU drain, so the
// CPU queues the right eye while the GPU still draws the left (DXVK tracks
// resource hazards itself). debug_keep_replay_drain=on restores the drain.
void install();
bool set_keep_replay_drain(const char *value);

// 10 s window report (render_dump.cpp poller); resets counters.
void report_window();

} // namespace mc2vr::pacing
