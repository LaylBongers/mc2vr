// Per-eye draw passes (docs/stereo.md).
//
// With frame_replay=on the frame is submitted twice. This module makes the
// two passes a stereo pair:
//   - eye_pass=on: pass 1 renders LEFT, pass 2 renders RIGHT — deterministic
//     per-frame eye selection via view_rewrite::set_pass_eye().
//   - Pass boundaries drive the eye captures: the engine's final frame of each
//     pass lands in the carrier-owned front surface (render_outputs.cpp),
//     captured into the shared ring at 1->2 (LEFT) and 2->0 (RIGHT). Pass 2
//     simply re-renders into the engine's own targets (pass 1 is already
//     composited and captured by then).
//   - debug_eye_dump_frames=N: after debug_dump_delay seconds, write BMP pairs
//     (mc2vr_eye_left/right_frame<N>.bmp) of the per-eye LDR finals for the
//     parallax check — pairs count only when NON-EMPTY and the per-eye
//     rewrite ran in both passes (others are skipped, retried at 0.5s).
//     tools/analyze_dumps.py reports the measured shift.
//
// Retired 2026-11-08 (carrier-owned outputs made them unnecessary): the
// pass-2 main-RT -> eye-RT redirect (eye_rt) and the monitor pin
// (eye_monitor_pin) — git history has them.
#pragma once

#include <cstdint>

namespace mc2vr::eye_replay {

// mc2vr.conf eye_pass=off|on (default off — hold-timer A/B stays in charge).
bool set_pass_enabled(const char *value);

// mc2vr.conf debug_eye_dump_frames=N (0 = never).
void set_dump_frames(uint32_t n);

// mc2vr.conf debug_dump_delay=S — shared with the stream dump window.
void set_dump_delay(float seconds);

// Pass tracking — called from stream_capture's SubmitToGPU hook around each
// invoke (0 = not inside a submit). Sets the per-pass eye-sign override
// (pass 1 = LEFT/-1, pass 2 = RIGHT/+1) when eye_pass=on.
void set_pass(uint32_t pass);
// Current pass: 0 = outside a submit, 1/2 while the passes run (render thread).
uint32_t current_pass();

// Called from device.cpp's Reset hook — the shared ring is dropped.
void on_reset();

// 10s window report (render_dump.cpp poller); resets counters.
void report_window();

} // namespace mc2vr::eye_replay
