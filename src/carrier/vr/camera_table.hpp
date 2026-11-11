// Union HMD camera injection at g_CameraTable — the single upstream point
// that steers the draw camera, culling and LOD together
// (docs/camera.md; plates on CameraTable_FillFromPose /
// CameraTable_FillLoop).
//
// g_CameraTable (0x014a2ee0) is 5 camera-entity slots x 0x620, filled once per
// frame by the CameraTable_FillFromPose loop (CameraTable_FillLoop, from
// InGameShellState_FramePipeline) — only slots with a live camera object
// (+0x1e0) get filled. The fill's Matrix_Copy3x4 (call 0x0070aef3) destination
// EAX = the slot's ACTIVE camera entry + 0x10. A MidHook right after that call
// (0x0070aef8) rewrites the JUST-filled entry with the game pose composed with
// the HMD UNION pose (mid-point between the eyes).
//
// Composition is PROBE-DERIVED (the entry's ROWS are the rendered camera's
// axes, and writes pass through the builder's 4x4 matrix inverse — a naive
// E·M lands inverted and world-side; closed form E' = S_r·L⁻¹·S_r·E — see
// the composition block in camera_table.cpp and the plate on
// CameraTable_FillFromPose; do not re-derive from static sign analysis).
// A runtime row-sign calibration locks the measured convention before any
// rewrite is applied.
//
// The write lands after every fill and before every consumer by construction:
// the draw-camera builder reads the table, and the culling volume is derived
// from the builder's frustum, so culling/LOD follow the head. The cull
// FOV (as opposed to its rotation) is cull_frustum.cpp's job.
//
// Pose source: the OpenXR host IPC state, sampled once per game frame. Without
// a tracked HMD the rewrite is a no-op.
//
// NOT per-eye and NOT a projection change: the per-eye projection (OpenXR FOV)
// + per-eye delta is view_rewrite's (hmd_delta) job at the record level —
// the intended pairing. Always on; without a tracked HMD it is a no-op.
#pragma once

#include <cstdint>

#include "vec_math.hpp"

namespace mc2vr::camera_table {

// True + fills *rot/*pos with the union pose the table rewrite used for the
// CURRENT game frame (main thread — the fill and the render passes share
// it). view_rewrite's hmd_delta mode uses it as the per-eye delta's
// reference so pose and delta always come from the same sample. False when
// not armed or the HMD wasn't tracked this frame.
bool get_union(math::Quat *rot, math::Vec3 *pos);

// Distance (m) and forward-axis angle (deg) of a camera entry (rows at 4-float
// stride, as in is_union_camera) from the newest rewritten (union) entry of
// the last few frames. False when nothing was rewritten recently. Diagnostics
// for views that don't match (cull_frustum's other-view report).
bool union_offset(const float *rows, const float *pos, float *dist_m, float *fwd_angle_deg);

// True + fills *rot/*pos with the union pose that was composed into the
// rewritten entry whose rendered basis (R, U, F — a decomposed VP camera)
// matches, searching the last few frames' rewrites. This is the union the VP
// ACTUALLY carries, so a per-eye delta taken against it is exact even if the
// game builds a frame's VP from an earlier frame's table fill. Main thread.
bool union_for_camera(const math::Vec3 &R, const math::Vec3 &U, const math::Vec3 &F,
                      math::Quat *rot, math::Vec3 *pos);

// True + fills *half_h/*half_v with the HMD FOV-union half-angles (radians,
// outermost bound over BOTH eyes) from a pose sampled within the
// last second. Input of the cull frustum (cull_frustum.hpp).
bool get_fov_union(float *half_h, float *half_v);

// True when a camera entry (rows = entry+0x10, the 3 rotation rows at a 4-float
// stride; pos = entry+0x40) equals one this module rewrote in the last few
// frames — i.e. the entry is an HMD-driven view (the builder works on copies
// of the table entries). Reflection/shadow/aux cameras never match. Callable
// from any thread.
bool is_union_camera(const float *rows, const float *pos);

// The fill site (hooks/camera_table.cpp), once per filled slot: rot = the
// just-filled entry+0x10 (3x3 rotation rows + position at +0x30, rewritten in
// place), slot = its 0x620 slot base.
void on_entry_filled(uintptr_t rot, uintptr_t slot);

// 10s window report (render_dump.cpp poller); resets counters.
void report_window();

} // namespace mc2vr::camera_table
