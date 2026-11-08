// debug_ps_census=on: per-pixel-shader GPU cost and pixel counts.
//   - One frame in kSampleEvery is measured (pass-1 start to the next pass-1
//     start). Inside it, every pixel-shader change (and every render-target
//     category / pass change) starts a segment.
//   - Per segment: GPU time (timestamp queries) and the pixels that passed the
//     depth test (occlusion query) — overdraw and per-pixel cost, not just time.
//   - Aggregated per pixel shader, identified by an FNV-1a hash of its bytecode
//     (GetFunction); tools/shader_hash.py maps hashes back to the game's shader
//     containers.
// Render thread only. Inert unless enabled (SetPixelShader + draw hooks are
// only installed then).
#pragma once

#include <cstdint>

namespace mc2vr::ps_census {

bool set_enabled(const char *value);
bool enabled();

void on_pass(uint32_t pass);  // eye_replay::set_pass entry: 1, 2, 0
void on_set_rt(uint32_t w, uint32_t h);
void on_set_ps(void *ps);
void on_draw();
void on_reset();

// 10 s window report (render_dump.cpp poller); resets counters.
void report_window();

} // namespace mc2vr::ps_census
