// debug_gpu_census=on: where does the GPU (and the render thread's submission
// time) go, per stereo pass and per render-target category?
//   - Segments: every SetRenderTarget(0) during a frame starts one, classified
//     by the target (main = engine render size, atlas = 1024x4096, square =
//     w == h, other = anything else); the carrier's own blits (eye capture,
//     monitor mirror) are marked explicitly.
//   - Per segment: draw calls, CPU submission time (QPC) and GPU time from
//     D3D9 timestamp queries, read back a few frames later without stalling.
//   - A frame spans pass-1 start to the next pass-1 start.
// Render thread only. Inert unless enabled (draw-call hooks are only
// installed then).
#pragma once

#include <cstdint>

namespace mc2vr::gpu_census {

bool set_enabled(const char *value);
bool enabled();

void on_pass(uint32_t pass);  // eye_replay::set_pass entry: 1, 2, 0
void on_set_rt(uint32_t w, uint32_t h);
void on_draw();
void carrier_begin();  // around the carrier's own GPU work
void carrier_end();
void on_reset();       // device Reset: queries are released

// 10 s window report (render_dump.cpp poller); resets counters.
void report_window();

} // namespace mc2vr::gpu_census
