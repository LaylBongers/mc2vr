// HMD cull frustum — make the engine's culling/LOD frustum cover the HMD's
// field of view instead of the game's widescreen one (docs/frustum_cull_plan.md).
//
// ViewContext_BuildCameraConstants (0x008591ac) converts the camera entry's
// fov into the two frustum half-extents tanH/tanV ONCE per view build. At
// 0x0085943B they sit in XMM2 (tanH) / XMM0 (tanV) — just stored to
// ctx+0x30/+0x34 — and EVERY frustum product of the build is derived from
// those two registers afterwards: the projection matrix (ctx+0xb20 -> VP ->
// g_ViewContextTable records), the four frustum-corner rays handed to the
// cull/cascade builders (FUN_00857140, FUN_0085a3f0 -> ctx+0x954/0xa70/0xa80,
// +0x46c/0x8c0/0x900), and the ctx tans the per-object cull divides by. A
// MidHook there replaces both extents with the HMD FOV union (+ margin) for
// HMD-driven views, so the whole culling volume is the HMD frustum by
// construction — no per-consumer chasing.
//
// Only views whose camera entry is a union-rewritten g_CameraTable copy are
// touched (camera_table::is_union_camera) — shadow/reflection/aux views keep the
// game's frustum. Never narrows: each extent is max(game, HMD).
//
// One consumer must NOT see the HMD frustum: the third-person camera sizes its
// obstacle clearance from the main view's near-plane quad (FUN_007107d0 copies
// the frustum struct and keeps the near diagonal/edge maxima). A second MidHook
// right after that copy (0x007107F9) scales the copy's near corners back to the
// game's own extents — without it the ~3x larger quad pulls the camera in.
//
// Side effects (all in the game's own projection, which the HMD never shows —
// view_row_rewrite=hmd_delta re-projects every main-pass upload with the
// per-eye OpenXR FOV): the decomposed `view: gameproj` line reports the HMD
// angles; shadow cascades are fitted to the wider frustum (coarser shadow
// texels); screen-size LOD metrics see the wider projection.
//
// Requires view_table_inject=on and a tracked HMD; otherwise inert.
#pragma once

namespace mc2vr::cull_frustum {

// mc2vr.conf cull_hmd_fov=on|off. Returns false on unrecognized input.
bool set_enabled(const char *value);

// mc2vr.conf cull_fov_margin=<degrees> — added to each HMD half-angle
// (default 5). Returns false out of range [0,30].
bool set_margin(double degrees);

// Install the builder MidHook (installed always; handler gated on the conf).
void install();

// 10s window report; resets counters.
void report_window();

} // namespace mc2vr::cull_frustum
