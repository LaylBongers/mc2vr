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

// mc2vr.conf view_row_rewrite=off|on|pulse|stereo|hmd|hmd_delta|hmd_identity
// (hmd = S4-4 full VP replacement from the HMD pose; hmd_delta = I1, the
// intended pairing with view_table_inject=on — per-eye projection from the
// OpenXR FOV + per-eye position delta, pose already in the records from the
// table union; hmd_identity = decompose/rebuild self-check, output must
// equal input). off = pass-through. stereo = per-eye offset
// D = ±right·view_ipd/2 (see docs/stereo_design.md §S2, handover step 1).
// Returns false on unrecognized values.
bool set_view_row_rewrite(const char *value);

// mc2vr.conf view_row_amp=<float>: pan amplitude in world units for on/pulse
// (default 4.0 = unmistakable; ~0.05 for game-scale checks, 0.032 = IPD).
void set_view_row_amp(float amp);

// mc2vr.conf view_ipd=<float>: full inter-pupillary distance in world units
// for stereo mode (default 0.065; per-eye offset is half of this).
void set_view_ipd(float ipd);

// mc2vr.conf view_stereo_hold=<float>: seconds each eye is held before
// alternating (stereo A/B verification until the S2c replay drives real
// per-eye passes; default 2.0).
void set_view_stereo_hold(float seconds);

// mc2vr.conf view_asym_x/y=<float>: per-eye asymmetric-projection centre
// shift in NDC units (default 0 = disabled; sign convention validated
// against the HMD runtime in S4).
void set_view_asym(float x, float y);

// mc2vr.conf view_world_scale=<float>: game world units per metre for the HMD
// camera (default 1.0 — UNVERIFIED, measure it; see docs/stereo_design.md §S4-4).
void set_view_world_scale(float units_per_metre);

// The world scale the camtable union injection composes with (shared conf key).
float world_scale();

// True while view_row_rewrite=hmd (the FULL HMD pose replacement at the
// upload) — camtable warns: that mode double-applies the pose on top of the
// table union (stereo_improvements_plan.md remaining work 4).
bool full_pose_rewrite_active();

// Pose id (host hostFrame+1) the CURRENT frame renders with, 0 when the frame
// is not rendered from an HMD pose. Constant across both passes of a frame;
// eye_share forwards it in FRAME_READY.
uint32_t current_pose_id();

// The RAW (pre-rewrite) game camera decomposed from the most recent
// viewContextData/ViewProj upload (hmd/hmd_identity modes only, main pass
// only). Matching input for the injection-probe tooling
// (src/carrier/debug/inject_probe.cpp) and the I1 implementation's
// consistency oracle: compares ViewEntry/camera-table state against the
// decomposed upload camera. Valid for ~1s after the last main-pass upload.
bool get_game_camera(vpcam::Camera *out);

// S2c-2 per-pass eye override (eye_replay.cpp): when nonzero (pass 1 = -1,
// pass 2 = +1) it replaces the view_stereo_hold A/B sign for the current
// draw pass — deterministic per-frame eye selection with the S2c replay.
// Zero restores the hold-timer behavior.
void set_pass_eye(int sign);

// 10s window report (called from render_dump.cpp's poller); resets counters.
void report_window();

} // namespace mc2vr::view
