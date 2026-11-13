// debug_gui_census=on: the draw-time view of the GUI (Scaleform) records, for
// the HUD fix (docs/hud.md, docs/reverse_engineering/gui_renderer.md). Every
// 30 s, one pass-1 frame: from 160 records before the first GUI record to the end
// of the sorted list, per record:
//   - the record fields (GUI or not, view type, view context, technique,
//     material, RT write mask, stencil mode), its build-time emitter (Submit
//     return address and three heuristic levels up) and its command stream;
//   - the render commands it executes (opcode + first operands);
//   - its vertex/pixel shader constant uploads (start register, count, first
//     values);
//   - its draws: count, render target / depth, depth and blend states.
// GUI records are marked at build time by debug_build_census's hooks
// (installed for this even with that census off). The 10 s poller prints the
// capture. Inert unless enabled.
//
// debug_gui_census=clouds: the same per-record view, but of the records the
// clouds pass emits (emitter inside PgClouds_EmitCloudRender1 /
// PgClouds_EmitCloudRender2), in both eye passes of two consecutive frames.
#pragma once

#include <windows.h>
#include <d3d9.h> // types only

#include <cstdint>

#include "game/pg_primitive.hpp"

namespace mc2vr::gui_census {

bool set_enabled(const char *value);
bool enabled();

// Taps (render thread).
void on_pass(uint32_t pass);                                  // eye_replay pass boundary
void on_record(uint16_t index, const game::PgPrimitive *rec);  // SubmitToGPU, before each record
void on_command(const uint32_t *cmd);                         // ExecuteStream, per command
void on_set_render_target(DWORD index, IDirect3DSurface9 *surface);
void on_stretch_rect(IDirect3DSurface9 *src, IDirect3DSurface9 *dst);
void on_set_depth(IDirect3DSurface9 *surface);
void on_shader_constants(bool pixel, UINT start, const float *data, UINT count);
// After the view rewrite of the same VS upload: out != in = it was rewritten.
void on_shader_constants_out(const float *in, const float *out, UINT count);
void on_render_state(D3DRENDERSTATETYPE state, DWORD value);
void on_draw();

// Prints a finished capture (render_dump.cpp poller).
void report_window();

} // namespace mc2vr::gui_census
