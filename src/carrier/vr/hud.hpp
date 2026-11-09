// The HUD as its own head-locked layer (docs/hud.md).
//
// The engine draws the flat HUD (HUD icons, text, frames, the Scaleform
// minimap/GUI) into each eye image at identical screen positions, which puts
// it ~14 cm in front of the face. Instead:
//   - Build (RenderShell_EndSubView, build thread): the record-index ranges of
//     the world markers and of the HUD are bracketed by the engine's record
//     write count, and published with the record list (PgPrimitive_PublishFrame
//     return, hooks/hud.cpp).
//   - Draw (PgPrimitive_SubmitToGPU, render thread), headset frames only:
//     pass 1 draws the HUD records once, after the record walk, into a
//     carrier-owned HUD target (render size, transparent, own depth-stencil
//     for the minimap's stencil mask); pass 2 skips them.
//   - Composite: after each pass the HUD texture is drawn into that eye's
//     image as a plane at hud_distance metres, hud_size degrees wide, centered
//     ahead of the head (head-locked), from the eye's FOV and the IPD.
// The world markers stay in the eye images (they depth-test against the scene).
#pragma once

#include <windows.h>
#include <d3d9.h> // types only

#include <cstdint>

namespace mc2vr::hud {

// conf hud_distance=<metres> (default 1.5), hud_size=<degrees wide> (default 55).
bool set_distance(const char *value);
bool set_size(const char *value);

// ---- build side (RenderShell_EndSubView) ----
void begin_markers();
void end_markers();
void begin_hud();
void end_hud();
// PgPrimitive_PublishFrame return (hooks/hud.cpp): the ranges of the list
// just published.
void on_publish();

// ---- draw side (PgPrimitive_SubmitToGPU, render thread) ----
// Frame start, after the pass-1 pose sample: whether this frame draws the HUD
// as a layer (a headset frame, the ranges match the drawn list, the target
// exists).
void begin_frame();
bool active();
bool is_hud_record(uint32_t index);
// Around drawing the HUD records into the HUD target (pass 1).
void begin_draw();
void end_draw();
// After a pass: the HUD into this pass's eye image (1 = LEFT, 2 = RIGHT).
void composite(uint32_t pass);

// ---- lifecycle (vr/render_outputs) ----
void ensure(IDirect3DDevice9 *device);
void on_reset_before();

// 10 s window report (render_dump.cpp poller); resets counters.
void report_window();

} // namespace mc2vr::hud
