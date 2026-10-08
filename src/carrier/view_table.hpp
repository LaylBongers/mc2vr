// Union HMD camera injection at g_CameraTable — the single upstream point
// that steers the draw camera, culling and LOD together
// (docs/stereo_improvements_plan.md; plates on CameraTable_FillFromPose /
// CameraTable_FillLoop).
//
// g_CameraTable (0x014a2ee0) is 5 camera-entity slots x 0x620, filled once per
// frame by the CameraTable_FillFromPose loop (FUN_0070f430, from
// InGameShellState_FramePipeline) — only slots with a live camera object
// (+0x1e0) get filled. The fill's Matrix_Copy3x4 (call 0x0070aef3) destination
// EAX = the slot's ACTIVE camera entry + 0x10. A MidHook right after that call
// (0x0070aef8) rewrites the JUST-filled entry with the game pose composed with
// the HMD UNION pose (mid-point between the eyes).
//
// Composition is PROBE-DERIVED (the entry's ROWS are the rendered camera's
// axes, and writes pass through the builder's 4x4 matrix inverse — a naive
// E·M lands inverted and world-side; closed form E' = S_r·L⁻¹·S_r·E — see
// the composition block in view_table.cpp and the plate on
// CameraTable_FillFromPose; do not re-derive from static sign analysis).
// A runtime row-sign calibration locks the measured convention before any
// rewrite is applied.
//
// The write lands after every fill and before every consumer by construction:
// the draw-camera builder path AND the culling/fov readers (0x0048067E family)
// both read the table, so culling/LOD follow the head (S6's original goal).
//
// Pose source: the OpenXR host IPC state, sampled once per game frame. Without
// a tracked HMD the rewrite is a no-op.
//
// NOT per-eye and NOT a projection change: the per-eye projection (OpenXR FOV)
// + per-eye delta is view_row_rewrite=hmd_delta's job at the record level —
// the intended pairing. stereo/hmd_identity are also compatible (the old
// `hmd` full-VP-replacement mode was removed 2026-10-07 — it double-applied
// the pose on top of this union).
#pragma once

#include "vec_math.hpp"

namespace mc2vr::camtable {

// mc2vr.conf view_table_inject=on|off. Returns false on unrecognized input.
bool set_inject_enabled(const char *value);

// mc2vr.conf debug_camtable_probe=on|off: transfer-function probe. Replaces
// the union rewrite with a fixed +10° rotation about one entry-local axis,
// cycling x→y→z every 2s, logging the pre/post entry and the previous
// frame's decomposed render result. One standing-still run measures the
// game's actual rotation convention (no HMD needed). Returns false on
// unrecognized input.
bool set_probe_enabled(const char *value);

// True + fills *rot/*pos with the union pose the table rewrite used for the
// CURRENT game frame (main thread — the fill and the render passes share
// it). view_rewrite's hmd_delta mode uses it as the per-eye delta's
// reference so pose and delta always come from the same sample. False when
// not armed or the HMD wasn't tracked this frame.
bool get_union(math::Quat *rot, math::Vec3 *pos);

// True + fills *half_h/*half_v with the HMD FOV-union half-angles (radians,
// BOTH eyes' outermost bounds) + cull_fov_margin degrees — the cull-fov
// widening inputs (frustum_cull_plan.md). Valid when the pose was sampled
// this game frame (requires view_table_inject=on). view_rewrite's
// record_fov_widen builds its projection from these.
bool get_fov_union(float *half_h, float *half_v);

// mc2vr.conf cull_fov_widen=on|off (frustum_cull_plan.md D0): MidHook at the
// ViewEntry fov-triple write scales the just-written cull fov to the HMD FOV
// union (+ margin), so the culling volume covers the whole HMD view. Without
// a tracked HMD it is a no-op. Returns false on unrecognized input.
bool set_fov_widen(const char *value);

// mc2vr.conf cull_fov_margin=<degrees> — extra half-angle added to the HMD
// FOV union before widening (default 5). Returns false on non-numbers.
bool set_fov_margin(double degrees);

// mc2vr.conf entry_fov_scale=<float> — causal probe on the camera-entry fov
// value (+0x58, engine units): 1.0 = observe only (census logging); >1
// scales the value at every entry fill (it feeds the projection tan-table
// index AND the 0x0048067E cull readers — if culling follows the scale, this
// is the operative cull channel). Returns false out of range (0,10].
bool set_entry_fov_scale(double scale);

// mc2vr.conf entry_fov_decouple=on|off: with entry_fov_scale active, repoint
// the camera-entry fillers' fov-constant loads at 1.0 (stock game
// projection/camera) while the cull derivations keep reading the wide
// patched constant. Returns false on unrecognized values.
bool set_entry_fov_decouple(const char *value);

// Install the fill-site MidHook (plaintext .text, single caller — the fill
// loop; fires once per filled slot) and, when cull_fov_widen is on, the
// fov-write MidHook. Handler no-ops unless the conf enabled
// it. Failure is non-fatal: the union injection stays idle.
void install();

// 10s window report (render_dump.cpp poller); resets counters.
void report_window();

} // namespace mc2vr::camtable
