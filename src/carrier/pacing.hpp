// Lockstep frame pacing with the host (docs/pacing.md). Main thread only.
//   - frame_start(): GameShell_FrameTick entry, BEFORE the game's own QPC
//     sample. While the host session is VISIBLE/FOCUSED: (1) wait (bounded
//     50 ms) until the host has published a pose newer than the one the last
//     frame rendered with; (2) feed the game's dt governor the DISPLAY time
//     delta of that pose instead of the FrameTick body time, so the
//     simulation advances exactly as far as the display does.
//   - on_pose_sampled(): the pass-1 pose sample's id (hostFrame+1).
#pragma once

#include <cstdint>

namespace mc2vr::pacing {

void frame_start();
void on_pose_sampled(uint32_t pose_id);

// The replay pass (pass 2) skips LtiRenderer_BeginSubmit's GPU drain, so the
// CPU queues the right eye while the GPU still draws the left (DXVK tracks
// resource hazards itself). debug_keep_replay_drain=on restores the drain.
void install();
bool set_keep_replay_drain(const char *value);

// 10 s window report (render_dump.cpp poller); resets counters.
void report_window();

} // namespace mc2vr::pacing
