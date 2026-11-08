// Per-eye camera channel at the GPU boundary (docs/view_rewrite.md,
// docs/camera.md): the camtable union injection puts the
// HMD-union pose into the game's camera table upstream; this module taps the
// SetVertexShaderConstantF uploads of the technique's viewContextData and
// applies the per-eye complement (hmd_delta; always on) or, with
// debug_view_identity=on, the decompose/rebuild self-check:
//   - install(): MidHook at the upload gate that publishes the current
//     technique's exact viewContextData register map.
//   - on_set_vs_constant(): the device VmtHook tap that applies the rewrite.
//   - on_set_render_target(): pass gate (RT0 size == engine render size) — only the main
//     scene pass is rewritten (shadow-map/reflection/offscreen passes are not).
//   - set_identity_oracle()/set_view_world_scale(): mc2vr.conf controls.
#pragma once

#include <cstdint>

#include "vp_camera.hpp"

namespace mc2vr::view_rewrite {

// Install the upload-gate MidHook. Failure is non-fatal (rewrite stays idle).
void install();

// Tap from device.cpp's SetVertexShaderConstantF handler (main thread, before
// the driver call). Returns the pointer the driver call must use: `data`
// itself, or a scratch copy carrying the rewrite.
const float *on_set_vs_constant(uint32_t start_register, const float *data,
                                uint32_t vec4_count);

// Render-target tracking (device.cpp's SetRenderTarget observer). The rewrite
// applies only while RT0 has the engine's render size (the main scene size).
// w=h=0 means no RT0.
void on_set_render_target(uint32_t w, uint32_t h);

// True when a render target of w x h is the engine's live render size (the
// main scene / per-eye chain size).
bool is_main_size(uint32_t w, uint32_t h);

// The rewrite is always hmd_delta: per-eye projection from the OpenXR FOV +
// per-eye position delta, the pose already in the records from the camera
// table union (camera_table.cpp). Without a tracked HMD it passes through.
// mc2vr.conf debug_view_identity=on: rebuild every main-pass VP with the
// game's OWN camera (output must equal input — the decomposition self-check;
// also the clean pass-through channel for debug_camtable_probe). Default off
// = the per-eye HMD rewrite (hmd_delta).
bool set_identity_oracle(const char *value);

// mc2vr.conf view_world_scale=<float>: game world units per metre for the HMD
// camera (default 1.0 — VERIFIED 2026-10-07: units are metres; see
// docs/reverse_engineering/world_units.md).
void set_view_world_scale(float units_per_metre);


// The world scale the camtable union injection composes with (shared conf key).
float world_scale();

// Pose id (host hostFrame+1) the CURRENT frame renders with, 0 when the frame
// is not rendered from an HMD pose. Constant across both passes of a frame;
// eye_share forwards it in FRAME_READY.
uint32_t current_pose_id();

// The RAW (pre-rewrite) game camera decomposed from the most recent
// viewContextData/ViewProj upload (main pass
// only). Input for the camtable row-sign calibration and the transfer probe;
// valid for ~1s after the last main-pass upload.
bool get_game_camera(vp_camera::Camera *out);

// Per-pass eye override (eye_replay.cpp): pass 1 = -1, pass 2 = +1,
// 0 = none. hmd_delta requires it (per-pass eye); the pass-1 transition
// samples the HMD pose once per frame.
void set_pass_eye(int sign);

// 10s window report (called from render_dump.cpp's poller); resets counters.
void report_window();

// Monotonic count of rewritten VP rows (render thread). A pass that leaves it
// unchanged drew with the game's own camera (no per-eye rewrite).
uint64_t rewrite_count();

} // namespace mc2vr::view_rewrite
