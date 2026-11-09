// The HUD as its own head-locked layer (docs/hud.md).
//
// The engine draws the flat HUD (HUD icons, text, frames, the Scaleform
// minimap/GUI) into each eye image at identical screen positions, which puts
// it ~14 cm in front of the face. Instead:
//   - Build (RenderShell_EndSubView, build thread): the record-index ranges of
//     the world markers and of the HUD are bracketed by the engine's record
//     write count, and published with the record list (PgPrimitive_PublishFrame
//     return, hooks/hud.cpp).
//   - Layout: while the layer is in use the GUI sees one fixed 16:9
//     kLayoutW x kLayoutH screen, on every thread, with no temporary swaps
//     (docs/reverse_engineering/view_blocks.md):
//       - the UI scale (view block +0x34, GUI-only) is kLayoutH/480 in every
//         block, written by our RenderShell_SetupViewports (its only writer,
//         reimplemented) and at the mode switch; the two eye-image readers (world and
//         pinned markers) get the stock H/480 back at their load;
//       - the aspect mode is WIDESCREEN;
//       - the GUI's W/H readers (widget layout, GUI screen info, off-screen
//         indicators, Scaleform's display rect) get kLayoutW x kLayoutH at
//         their loads (hooks/hud.cpp); the scene and camera keep the real size.
//   - Draw (PgPrimitive_SubmitToGPU, render thread), headset frames only:
//     pass 1 draws the HUD records once, after the record walk, into a
//     carrier-owned kLayoutW x kLayoutH target (transparent, own
//     depth-stencil for the minimap's stencil mask) with that size's viewport
//     and pixel-to-clip constants; pass 2 skips them.
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

// The HUD layer's layout size.
constexpr uint32_t kLayoutW = 1920, kLayoutH = 1080;

// The same as float bit patterns: hook handlers write these instead of doing
// float math, since some sites have live x87 state (hooks/hud.cpp).
constexpr uint32_t kLayoutWBits = 0x44f00000u;  // 1920.0f
constexpr uint32_t kLayoutHBits = 0x44870000u;  // 1080.0f
constexpr uint32_t kUiScaleBits = 0x40100000u;  // 2.25f = kLayoutH / 480

// ---- layout mode ----
// The GUI sees the layer's screen (any thread).
bool layout_16x9();
// Once per update tick (FrameTick PostUpdateHook site): on a change of mode
// (HMD in use — render_size::hmd_in_use — and the layer target exists), apply
// the screen policy and make the GUI manager re-lay out every widget on its
// next update.
void on_update_tick();
// End of our RenderShell_SetupViewports (reimpl/render/setup_viewports.cpp):
// the stock view blocks were just written — remember their UI scales, apply
// the policy.
void on_viewports_set(uint8_t *shell);
// After SetupRenderTargets / a device restore (vr/render_outputs): the engine
// re-snaps the aspect mode there.
void on_screen_setup();
// The stock UI scale (H/480 float bits) of view block `index`, 0 if unknown:
// for the readers that draw into the eye image (world / pinned markers).
uint32_t eye_scale_bits(uint32_t index);

// ---- build side (RenderShell_EndSubView) ----
void begin_markers();
void end_markers();
// Around the HUD steps (records the layout the build ran with).
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
