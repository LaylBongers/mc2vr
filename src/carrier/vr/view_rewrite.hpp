// Per-eye camera channel at the GPU boundary (docs/view_rewrite.md,
// docs/camera.md): the camtable union injection puts the
// HMD-union pose into the game's camera table upstream; this module taps the
// SetVertexShaderConstantF uploads of the technique's viewContextData and
// applies the per-eye complement (hmd_delta; always on) or, with
// debug_view_identity=on, the decompose/rebuild self-check:
//   - on_upload_gate(): called by the PgPrimitive_SubmitToGPU reimplementation at
//     each viewContextData upload; publishes the technique's exact register map.
//   - on_set_vs_constant(): the device VmtHook tap that applies the rewrite.
//   - on_set_render_target(): pass gate (RT0 size == engine render size) — only the main
//     scene pass is rewritten (shadow-map/reflection/offscreen passes are not).
//   - set_identity_oracle()/set_view_world_scale(): mc2vr.conf controls.
#pragma once

#include "vr/xr_session.hpp"

#include <windows.h>
#include <d3d9.h>

#include <cstdint>

#include "vr/camera_table.hpp"
#include "vr/vp_camera.hpp"

namespace mc2vr::view_rewrite {

// The constant-upload gate (the PgPrimitive_SubmitToGPU reimplementation, at
// the original's 0x00855a78), render thread, right before the wrapper ->
// device call: tech = the CURRENT technique object
// (0 = none), rec = the pass's viewContext record pointer. Publishes the
// technique's constant map for on_set_vs_constant.
void on_upload_gate(uintptr_t tech, uintptr_t rec);

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

// The view builder finished an HMD-driven view (reimpl/render/
// build_camera_constants.cpp): vp = its VP (16 floats, D3DX layout; every
// record of the view copies it, and the upload carries its transpose), with the HMD sample and union pose its camera was
// rewritten with. The per-eye rewrite looks uploads up by exact VP equality.
void on_hmd_view_built(const float *vp, const camera_table::UnionSample &sample, const math::Quat &rot,
                       const math::Vec3 &pos);

// True while RT0 has the main scene size: the passes the rewrite applies to.
bool main_pass_active();

// The main view camera's pitch in degrees (world +y up) as last seen by the
// rewrite (pass 1), and whether it has been seen. Diagnostics.
bool main_camera_pitch(float *degrees);

// The vertex shader the next record draws with (the PgPrimitive_SubmitToGPU
// reimplementation, before the record's command stream runs): its explicit
// camera matrices (LocalToProj, g_ViewProjMtx in clip space; LocalToView in
// view space), if any, get this pass's per-eye correction when uploaded.
void on_record_shader(IDirect3DVertexShader9 *vs);

// A PS constant upload from the current record's command stream
// (RenderCmd_ExecuteStream, SetPSConstants): in a LocalToView record, the
// pixel shader's ViewToLocal gets this pass's per-eye correction. Returns the
// data to upload (a scratch copy when rewritten).
const float *on_record_ps_constant(uint32_t start_register, const float *data, uint32_t vec4_count);

// The rewrite is always hmd_delta: per-eye projection from the OpenXR FOV +
// per-eye position delta, the pose already in the records from the camera
// table union (camera_table.cpp). Without a tracked HMD it passes through.
// mc2vr.conf debug_view_identity=on: rebuild every main-pass VP with the
// game's OWN camera (output must equal input — the decomposition self-check).
// Default off
// = the per-eye HMD rewrite (hmd_delta).
bool set_identity_oracle(const char *value);

// mc2vr.conf view_world_scale=<float>: game world units per metre for the HMD
// camera (default 1.0 — VERIFIED 2026-10-07: units are metres; see
// docs/reverse_engineering/world_units.md).
void set_view_world_scale(float units_per_metre);


// The world scale the camtable union injection composes with (shared conf key).
float world_scale();

// The stereo frame's start (eye_replay::begin_left_pass): the frame's views
// from the OpenXR session (nullptr = none). Returns the views both passes
// render with: these, the previous ones held for a short tracking drop, or
// nullptr (no HMD pose: the game's own camera). Valid until the next call.
const xr_session::ViewState *begin_frame(const xr_session::ViewState *views);

// Pose id (xr_session ViewState::pose_id) the CURRENT frame renders with, 0 when the frame
// is not rendered from an HMD pose. Constant across both passes of a frame.
uint32_t current_pose_id();

// The two eye poses (LEFT, RIGHT) the current frame renders with (from
// begin_frame). False when the frame has no HMD pose (current_pose_id() == 0).
bool frame_eyes(vp_camera::EyePose out[2]);

// The RAW (pre-rewrite) game camera decomposed from the most recent
// viewContextData/ViewProj upload (main pass
// only). Input for the camtable row-sign calibration and the transfer probe;
// valid for ~1s after the last main-pass upload.
bool get_game_camera(vp_camera::Camera *out);

// The current pass's mono -> eye clip-space map (EyeVP · MonoVP⁻¹, the same
// delta applied to the scene): out = delta · in. False before the pass's
// first main-pass camera upload or without an HMD pose.
bool mono_clip_to_eye(const double in[4], double out[4]);

// Per-pass eye override (eye_replay.cpp): pass 1 = -1, pass 2 = +1,
// 0 = none. hmd_delta requires it (per-pass eye).
void set_pass_eye(int sign);

// 10s window report (called from render_dump.cpp's poller); resets counters.
void report_window();

// Monotonic count of rewritten VP rows (render thread). A pass that leaves it
// unchanged drew with the game's own camera (no per-eye rewrite).
uint64_t rewrite_count();

} // namespace mc2vr::view_rewrite
