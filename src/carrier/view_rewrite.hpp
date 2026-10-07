// Per-eye camera channel at the GPU boundary (docs/stereo_design.md §S4-4,
// ../stereo_improvements_plan.md): the camtable union injection puts the
// HMD-union pose into the game's camera table upstream; this module taps the
// SetVertexShaderConstantF uploads of the technique's viewContextData and
// applies the per-eye complement (hmd_delta) or the decompose/rebuild
// self-check (hmd_identity):
//   - install(): MidHook at the upload gate that publishes the current
//     technique's exact viewContextData register map.
//   - on_set_vs_constant(): the device VmtHook tap that applies the rewrite.
//   - on_set_render_target()/set_main_rt_size(): pass gate — only the main
//     scene pass is rewritten (shadow-map/reflection/offscreen passes are not).
//   - set_view_row_rewrite()/set_view_world_scale(): mc2vr.conf controls.
#pragma once

#include <cstdint>

#include "vp_camera.hpp"

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

// mc2vr.conf view_row_rewrite=off|hmd_delta|hmd_identity (hmd_delta = I1,
// the pairing with view_table_inject=on — per-eye projection from the
// OpenXR FOV + per-eye position delta, pose already in the records from the
// table union; hmd_identity = decompose/rebuild self-check, output must
// equal input, and the clean pass-through channel for probe runs; off =
// pass-through). The old `hmd` full-VP mode and the S2 verification modes
// (on/pulse/stereo) were REMOVED 2026-10-07 — superseded by
// view_table_inject + hmd_delta. Returns false on unrecognized values.
bool set_view_row_rewrite(const char *value);

// mc2vr.conf view_world_scale=<float>: game world units per metre for the HMD
// camera (default 1.0 — VERIFIED 2026-10-07: units are metres; see
// docs/reverse_engineering/pandemic_engine.md § World units).
void set_view_world_scale(float units_per_metre);

// The world scale the camtable union injection composes with (shared conf key).
float world_scale();

// Pose id (host hostFrame+1) the CURRENT frame renders with, 0 when the frame
// is not rendered from an HMD pose. Constant across both passes of a frame;
// eye_share forwards it in FRAME_READY.
uint32_t current_pose_id();

// The RAW (pre-rewrite) game camera decomposed from the most recent
// viewContextData/ViewProj upload (hmd_delta/hmd_identity modes, main pass
// only). Input for the camtable row-sign calibration and the transfer probe;
// valid for ~1s after the last main-pass upload.
bool get_game_camera(vpcam::Camera *out);

// S2c-2 per-pass eye override (eye_replay.cpp): pass 1 = -1, pass 2 = +1,
// 0 = none. hmd_delta requires it (per-pass eye); the pass-1 transition
// samples the HMD pose once per frame.
void set_pass_eye(int sign);

// 10s window report (called from render_dump.cpp's poller); resets counters.
void report_window();

} // namespace mc2vr::view
