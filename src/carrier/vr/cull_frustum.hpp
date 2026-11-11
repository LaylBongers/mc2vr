// HMD cull frustum — make the engine's culling/LOD frustum cover the HMD's
// field of view instead of the game's widescreen one (docs/culling.md).
//
// ViewContext_BuildCameraConstants (0x008591ac) converts the camera entry's
// fov into the two frustum half-extents tanH/tanV ONCE per view build. At
// 0x0085943B they sit in XMM2 (tanH) / XMM0 (tanV) — just stored to
// ctx+0x30/+0x34 — and EVERY frustum product of the build is derived from
// those two registers afterwards: the projection matrix (ctx+0xb20 -> VP ->
// g_ViewContextTable records), the four frustum-corner rays handed to the
// cull/cascade builders (Frustum_Build, ViewContext_BuildCascadeFrusta -> ctx+0x954/0xa70/0xa80,
// +0x46c/0x8c0/0x900), and the ctx tans (read by render constants and the
// snapshot only — writing them alone moved no culling, docs/culling.md). A
// MidHook there replaces both extents with the HMD FOV union for
// HMD-driven views, so the whole culling volume is the HMD frustum by
// construction — no per-consumer chasing.
//
// Only views whose camera entry is a union-rewritten g_CameraTable copy are
// touched (camera_table::is_union_camera) — shadow/reflection/aux views keep the
// game's frustum. Never narrows: each extent is max(game, HMD).
//
// One consumer must NOT see the HMD frustum: the third-person camera sizes its
// obstacle clearance from the main view's near-plane quad (CamCtrl_NearPlaneClearance copies
// the frustum struct and keeps the near diagonal/edge maxima). A second MidHook
// right after that copy (0x007107F9) rescales the copy's near corners to the
// game's fov at a 16:9 reference aspect — without it the ~3x larger quad pulls
// the camera in. (Not the game's own extents: in VR the engine builds at the
// eye's render aspect, whose quad is narrower than the 16:9 one the camera was
// tuned against.)
//
// Side effects (all in the game's own projection, which the HMD never shows —
// view_rewrite (hmd_delta) re-projects every main-pass upload with the
// per-eye OpenXR FOV): the decomposed `view: gameproj` line reports the HMD
// angles; shadow cascades are fitted to the wider frustum (coarser shadow
// texels); screen-size LOD metrics see the wider projection.
//
// Always on; inert without a tracked HMD (the camera-table union is absent).
#pragma once

#include <cstdint>

namespace mc2vr::cull_frustum {


// The view builder's frustum extents are ready (hooks/cull_frustum.cpp):
// vctx = the view render-ctx, th/tv = tanH/tanV (in/out). For HMD-driven
// views widens both (also stored to the ctx tans); true = write them back.
bool on_tans_ready(uintptr_t vctx, float &th, float &tv);

// Third-person camera clearance (camera thread): `frustum` = the camera's
// private stack copy of the main view's frustum struct; rescales its near
// corners to the 16:9 reference extents.
void on_camera_frustum_copied(float *frustum);

// 10s window report; resets counters.
void report_window();


} // namespace mc2vr::cull_frustum
