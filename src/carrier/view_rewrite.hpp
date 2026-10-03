// Per-eye camera injection at the GPU boundary (docs/stereo_design.md §S2).
//
// The draw camera is not patchable in plaintext game data; it reaches the GPU
// only as the `viewContextData` vertex-shader constants. This module rewrites
// those registers inside the SetVertexShaderConstantF upload (on a copy — the
// game's buffer is never modified):
//   - install(): MidHook at the upload gate that publishes the current
//     technique's exact viewContextData register map.
//   - on_set_vs_constant(): the device VmtHook tap that applies the rewrite.
//   - on_set_render_target()/set_main_rt_size(): pass gate — only the main
//     scene pass is rewritten (shadow-map/reflection/offscreen passes are not).
//   - set_view_row_rewrite()/set_view_row_amp(): mc2vr.conf controls
//     (off | on | pulse verification modes).
#pragma once

#include <cstdint>

namespace mc2vr::view {

// Install the upload-gate MidHook. Failure is non-fatal (rewrite stays idle).
void install();

// Tap from device.cpp's SetVertexShaderConstantF handler (main thread, before
// the driver call). Returns the pointer the driver call must use: `data`
// itself, or a scratch copy carrying the rewrite.
const float *on_set_vs_constant(uint32_t start_register, const float *data,
                                uint32_t vec4_count);

// Render-target tracking (device.cpp's SetRenderTarget observer). The rewrite
// applies only while RT0 has the main scene size (the backbuffer size).
// w=h=0 means no RT0.
void on_set_render_target(uint32_t w, uint32_t h);
void set_main_rt_size(uint32_t w, uint32_t h);

// mc2vr.conf view_row_rewrite=off|on|pulse. off = pass-through. Returns false
// on unrecognized values.
bool set_view_row_rewrite(const char *value);

// mc2vr.conf view_row_amp=<float>: pan amplitude in world units for on/pulse
// (default 4.0 = unmistakable; ~0.05 for game-scale checks, 0.032 = IPD).
void set_view_row_amp(float amp);

// 10s window report (called from render_dump.cpp's poller); resets counters.
void report_window();

} // namespace mc2vr::view
