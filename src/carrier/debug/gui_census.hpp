// debug_gui_census=on: the draw-time view of the GUI (Scaleform) records, for
// the HUD fix (docs/hud.md, docs/reverse_engineering/gui_renderer.md). Every
// 30 s, one pass-1 frame: from 160 records before the first GUI record to the end
// of the sorted list, per record:
//   - the record fields (GUI or not, view type, view context, technique,
//     material, RT write mask, stencil mode);
//   - the render commands it executes (opcode + first operands);
//   - its vertex/pixel shader constant uploads (start register, count, first
//     values);
//   - its draws: count, render target / depth, depth and blend states.
// GUI records are marked at build time by debug_build_census's hooks
// (installed for this even with that census off). The 10 s poller prints the
// capture. Inert unless enabled.
#pragma once

#include <windows.h>
#include <d3d9.h> // types only

#include <cstdint>

#include "game/pg_primitive.hpp"

namespace mc2vr::gui_census {

bool set_enabled(const char *value);
bool enabled();

// debug_gui_skip=cycle (with the census on): from 48 records before the first
// GUI record to the end of the list, skip one record class at a time, 10 s
// each, both eyes; each phase is logged ("gui: skip phase N"). For telling the
// flat HUD, the world markers and the rest apart by what disappears.
// debug_gui_skip=each: skip the k-th engine-2D (technique-73) record after the
// composite in phase k (1..12, 5 s each; phase 0 = reference). The mirror
// shows the phase as squares. debug_gui_skip=markers: skip every record
// emitted under Hud_DrawWorldMarkers (FUN_0060f610). debug_gui_skip=callers:
// phase k (1..4, 10 s each) hides the records emitted under the k-th other
// HUD caller. All change rendering.
bool set_skip(const char *value);
// SubmitToGPU, per record, both passes: true = do not submit it.
bool skip_record(uint16_t index, const game::PgPrimitive *rec);
// render_outputs, after each monitor-mirror refresh: during skip phase N, N
// white squares in the mirror's top-left corner (monitor only, not the HMD).
void on_mirror_refreshed(IDirect3DDevice9 *device, IDirect3DSurface9 *mirror);

// Taps (render thread).
void on_pass(uint32_t pass);                                  // eye_replay pass boundary
void on_record(uint16_t index, const game::PgPrimitive *rec);  // SubmitToGPU, before each record
void on_command(const uint32_t *cmd);                         // ExecuteStream, per command
void on_set_render_target(DWORD index, IDirect3DSurface9 *surface);
void on_stretch_rect(IDirect3DSurface9 *src, IDirect3DSurface9 *dst);
void on_set_depth(IDirect3DSurface9 *surface);
void on_shader_constants(bool pixel, UINT start, const float *data, UINT count);
void on_render_state(D3DRENDERSTATETYPE state, DWORD value);
void on_draw();

// Prints a finished capture (render_dump.cpp poller).
void report_window();

} // namespace mc2vr::gui_census
