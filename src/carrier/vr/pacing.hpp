// Lockstep frame pacing with the OpenXR frame loop (docs/pacing.md).
//   - on_frame(): render thread, the stereo frame's start: records the frame's
//     display time and period while the session shows frames. The OpenXR
//     frame loop itself paces the render thread (xr_session.hpp); the update
//     thread is never held.
//   - frame_start(): update thread, GameShell_FrameTick entry, before the
//     game's QPC sample: feeds the dt governor the display-time step (last
//     rendered display time + one display period, minus the previous target).
#pragma once

#include <cstdint>

#include "vr/xr_session.hpp"

namespace mc2vr::pacing {

void frame_start();
void on_frame(const xr_session::Frame &xr);

// The replay pass (pass 2) skips LtiRenderer_BeginSubmit's GPU drain, so the
// CPU queues the right eye while the GPU still draws the left (DXVK tracks
// resource hazards itself). on_drain_check_busy(): BeginSubmit's drain check saw the GPU busy
// (called by reimpl/render/begin_submit.cpp); true = report it idle (skip
// the drain).
bool on_drain_check_busy();

// 10 s window report (render_dump.cpp poller); resets counters.
void report_window();

} // namespace mc2vr::pacing
