// S2c-2: per-eye draw passes (docs/s2c_handover.md, docs/stereo_design.md §S2c).
//
// With frame_replay=on (S2c-1) the frame is submitted twice. This module makes
// the two passes a real stereo pair:
//   - eye_pass=on:  pass 1 renders LEFT, pass 2 renders RIGHT — deterministic
//     per-frame eye selection via view::set_pass_eye() (replaces the
//     view_stereo_hold A/B timer while a pass override is active).
//   - eye_rt=on:    during pass 2 every device SetRenderTarget(0, mainRT) is
//     redirected to a carrier-created backbuffer-sized eye render target, and
//     StretchRect sources pointing at the main RT are redirected too (pass-2
//     post-effects must read the pass-2 accumulation, not pass 1's frozen
//     final). The game's caller-side RT cache is untouched — device-level
//     substitution only (the caches live in the Dx9_* callers; EndSubmit reads
//     g_CurRenderTarget, so its RT0->backbuffer StretchRect keeps copying the
//     pass-1 image to the monitor — the screen shows pass 1's eye, stable).
//   - eye_dump_frames=N: after stream_dump_delay seconds, write BMP dumps of
//     both passes' targets (left = the game's main RT after pass 1, right =
//     the eye RT after pass 2) for the parallax check (tools/analyze_dumps.py).
//
// Depth is shared with the game (BeginSubmit clears RT0+depth each pass, so
// each pass starts fresh). The eye RT is invalidated on device Reset.
#pragma once

#include <cstdint>

namespace mc2vr::eye {

// mc2vr.conf eye_pass=off|on (default off — hold-timer A/B stays in charge).
bool set_pass_enabled(const char *value);

// mc2vr.conf eye_rt=off|on (default off — pass 2 draws wherever the game draws).
bool set_rt_enabled(const char *value);

// mc2vr.conf eye_dump_frames=N (0 = never; dumps need eye_rt=on).
void set_dump_frames(uint32_t n);

// mc2vr.conf stream_dump_delay=S — shared with the stream dump window.
void set_dump_delay(float seconds);

// Pass tracking — called from stream_capture's SubmitToGPU hook around each
// invoke (0 = not inside a submit). Sets the per-pass eye-sign override
// (pass 1 = LEFT/-1, pass 2 = RIGHT/+1) when eye_pass=on.
void set_pass(uint32_t pass);

// Called from device.cpp's SetRenderTarget hook BEFORE the original call.
// Returns the surface the device call must use: `game_surface` itself, or
// the eye RT (pass-2 redirect of the main scene target, slot 0 only).
void *on_set_render_target(void *device, uint32_t index, void *game_surface);

// Called from device.cpp's StretchRect hook BEFORE the original call, for the
// SOURCE surface only. Same substitution rule as on_set_render_target.
void *on_stretch_src(void *game_src);

// Called from device.cpp's Reset hook — surfaces are lost; forget them.
void on_reset();

// 10s window report (render_dump.cpp poller); resets counters.
void report_window();

} // namespace mc2vr::eye
