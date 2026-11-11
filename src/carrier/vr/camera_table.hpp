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

// Which HMD sample a rewritten entry's union pose came from.
struct UnionSample {
    uint32_t pose_id = 0;      // xr_session ViewState::pose_id at the table fill
    int64_t display_time = 0;  // its predicted display time, ns
};

// True + fills *half_h/*half_v with the HMD FOV-union half-angles (radians,
// outermost bound over BOTH eyes) from a pose sampled within the
// last second. Input of the cull frustum (cull_frustum.hpp).
bool get_fov_union(float *half_h, float *half_v);

// True when the camera entry whose rows are at `rows` (entry+0x10, an address
// inside g_CameraTable) was rewritten with the HMD pose in the last few
// frames; *sample (optional) = which HMD sample. An exact address test: the
// view builder resolves its camera object to the g_CameraTable entry it comes
// from (vr/cull_frustum.cpp). Callable from any thread.
// *rot/*pos (optional) = the union pose composed into it.
bool rewritten_entry(uintptr_t rows, UnionSample *sample = nullptr, math::Quat *rot = nullptr,
                     math::Vec3 *pos = nullptr);

// The fill site (hooks/camera_table.cpp), once per filled slot: rot = the
// just-filled entry+0x10 (3x3 rotation rows + position at +0x30, rewritten in
// place), slot = its 0x620 slot base.
void on_entry_filled(uintptr_t rot, uintptr_t slot);

// 10s window report (render_dump.cpp poller); resets counters.
void report_window();

} // namespace mc2vr::camera_table
