// Lockstep frame pacing with the OpenXR frame loop (docs/pacing.md).
//   - render_gate(): render thread, at the pass-1 pose sample. While the
//     session is VISIBLE/FOCUSED, waits (bounded 50 ms) until the session
//     thread has published views newer than the ones the last frame rendered
//     with, and returns them. The update thread is never held, so the next
//     frame's update and render prep overlap the session thread's
//     xrEndFrame / xrWaitFrame.
//   - frame_start(): update thread, GameShell_FrameTick entry, before the
//     game's QPC sample: feeds the dt governor the display-time step (last
//     rendered display time + one display period, minus the previous target).
#pragma once

#include <cstdint>

#include "vr/xr_session.hpp"

namespace mc2vr::pacing {

void frame_start();
// False when VR is off for the run.
bool render_gate(xr_session::ViewState *st);

// The replay pass (pass 2) skips LtiRenderer_BeginSubmit's GPU drain, so the
// CPU queues the right eye while the GPU still draws the left (DXVK tracks
// resource hazards itself). debug_keep_replay_drain=on restores the drain.
// on_drain_check_busy(): BeginSubmit's drain check saw the GPU busy
// (called by reimpl/render/begin_submit.cpp); true = report it idle (skip
// the drain).
bool on_drain_check_busy();
bool keep_replay_drain();
bool set_keep_replay_drain(const char *value);

// 10 s window report (render_dump.cpp poller); resets counters.
void report_window();

} // namespace mc2vr::pacing
