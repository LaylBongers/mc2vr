// Union HMD camera injection at g_CameraTable — docs/stereo_improvements_plan.md
// "Decided architecture" (remaining work 1; the fill loop mapped statically
// 2026-10-07, see remaining work 2 there).
//
// g_CameraTable (0x014a2ee0) is 5 camera-entity slots x 0x620, filled once per
// frame by the CameraTable_FillFromPose loop (FUN_0070f430, from
// InGameShellState_FramePipeline) — only slots with a live camera object
// (+0x1e0) get filled. The fill's Matrix_Copy3x4 (call 0x0070aef3 ->
// 0x00836120) destination EAX = the slot's ACTIVE camera entry + 0x10
// (self-indexed: slot + [slot+4]*0x70 + 0x10). A MidHook at the instruction
// right after that call (0x0070aef8) rewrites the JUST-filled entry — rotation
// rows at EAX+0x00/+0x10/+0x20 and position at EAX+0x30 — with the game pose
// composed with the HMD UNION pose (mid-point between the eyes): the same
// body+offset model as vpcam::apply_eye, minus the projection terms (those
// live at the viewContextData record level, not in the table).
//
// The write lands after every fill and before every consumer by construction:
// the draw-camera builder path (ViewContext_BuildCameraConstants via the
// builder's stack entry) AND the culling/fov readers (0x0048067E family) both
// read the table, so culling/LOD follow the head (S6's original goal).
//
// Pose source: the OpenXR host IPC state, sampled once per game frame (the
// fill runs in the game-logic phase; same one-frame pose cadence the S4-4
// record path accepts). Without a tracked HMD the rewrite is a no-op.
//
// NOT per-eye and NOT a projection change: the per-eye projection (OpenXR
// FOV) + per-eye position delta is view_row_rewrite=hmd_delta's job at the
// record level (I1) — that is the intended pairing. view_row_rewrite=hmd
// re-applies the FULL HMD pose at the upload on top of the union (pose
// doubled); stereo/hmd_identity are also compatible.
#pragma once

#include "vec_math.hpp"

namespace mc2vr::camtable {

// mc2vr.conf view_table_inject=on|off. Returns false on unrecognized input.
bool set_inject_enabled(const char *value);

// True + fills *rot/*pos with the union pose the table rewrite used for the
// CURRENT game frame (main thread — the fill and the render passes share
// it). view_rewrite's hmd_delta mode uses it as the per-eye delta's
// reference so pose and delta always come from the same sample. False when
// not armed or the HMD wasn't tracked this frame.
bool get_union(math::Quat *rot, math::Vec3 *pos);

// Install the fill-site MidHook (plaintext .text, single caller — the fill
// loop; fires once per filled slot). Handler no-ops unless the conf enabled
// it. Failure is non-fatal: the union injection stays idle.
void install();

// 10s window report (render_dump.cpp poller); resets counters.
void report_window();

} // namespace mc2vr::camtable
