// S2c-2: per-eye draw passes (docs/s2c_handover.md, docs/stereo_design.md §S2c).
//
// With frame_replay=on (S2c-1) the frame is submitted twice. This module makes
// the two passes a real stereo pair:
//   - eye_pass=on:  pass 1 renders LEFT, pass 2 renders RIGHT — deterministic
//     per-frame eye selection via view::set_pass_eye() (replaces the
//     view_stereo_hold A/B timer while a pass override is active).
//   - eye_rt=on:    during pass 2 every device SetRenderTarget(0, mainRT) is
//     redirected to a carrier-created backbuffer-sized eye render target, and
//     StretchRect/UpdateSurface sources pointing at the main RT are redirected
//     too (pass-2 post-effects must read the pass-2 accumulation, not pass
//     1's frozen final; EndSubmit's final copy likewise carries pass 2's image
//     to the monitor — L/R alternate per frame unless eye_monitor_pin=on).
//     The game's caller-side RT cache is untouched — device-level
//     substitution only.
//   - eye_dump_frames=N: after stream_dump_delay seconds, write BMP pairs
//     (mc2vr_eye_left/right_frame<N>.bmp) for the parallax check — pairs count
//     only when NON-EMPTY (black loading/video frames are skipped and retried
//     at a 0.5s cadence, so the window waits for real content). fp16 HDR
//     targets (A16B16G16R16F) are decoded + Reinhard-tonemapped to 24-bit;
//     tools/analyze_dumps.py reports the measured shift.
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

// mc2vr.conf eye_monitor_pin=off|on (default off). When on, pass 2 never
// touches the backbuffer: StretchRect/UpdateSurface writes into it are
// skipped, and SetRenderTarget(0, backbuffer) is redirected to a carrier
// sink RT (gameplay's final composite is a DRAW, not a blit — proven by the
// 2026-10-04 run: zero pass-2 StretchRects into the backbuffer while the
// monitor still alternated). The monitor keeps pass 1's LEFT image — the S4
// steady state (the compositor consumes the eye RT).
bool set_pin_enabled(const char *value);

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

// Called from device.cpp's StretchRect hook BEFORE the original call, for
// the SOURCE surface. Same substitution rule as on_set_render_target; sets
// *skip when the monitor pin is active and this blit writes the backbuffer
// in pass 2 (the hook then returns S_OK without calling the original,
// keeping pass 1's image on the monitor — any source, not just the main RT:
// gameplay's final hop comes from an intermediate post surface).
void *on_stretch_src(void *game_src, void *dst, bool *skip);

// Called from device.cpp's UpdateSurface hook (slot 30) — same pass-2 rules
// as on_stretch_src (pin skips backbuffer writes; main-RT sources read the
// eye RT instead).
void *on_update_surface_src(void *game_src, void *dst, bool *skip);

// Called from device.cpp's UpdateTexture hook (slot 31) — diagnostic only
// (the source is a texture and can't be matched against the RT surface).
void on_update_texture(void *src_tex, void *dst_tex);

// Called from device.cpp's Reset hook — surfaces are lost; forget them.
void on_reset();

// 10s window report (render_dump.cpp poller); resets counters.
void report_window();

} // namespace mc2vr::eye
