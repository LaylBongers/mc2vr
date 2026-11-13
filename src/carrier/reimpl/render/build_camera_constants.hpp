// ViewContext_BuildCameraConstants (entry 0x008591a0, body 0x008591ac) —
// reimplementation.
//
// STATUS: live; matches the original byte for byte on every output (stock
// behavior).
// Ghidra plate: ViewContext_BuildCameraConstants.
// Original: stdcall(ctx), `ret 4`. The plaintext prologue at 0x008591a0 jumps
// through the call gate [0x024cd494] -> 0x00506a26 (sub esp,0xa4) into the
// body; RenderShell_BuildSceneRecords calls it once per active view.
// Entry: game/view_context.hpp's declaration (hooks/reimpl.cpp).
//
// Builds one view's camera constants (docs/reverse_engineering/view_frustum.md):
// the viewport, the view matrix (the camera entry inverted), the frustum
// half-extents tanH/tanV, the projection and VP, the view and shadow frusta
// (Frustum_Build) with the four cascade splits (ViewContext_BuildCascadeFrusta),
// then the frame's g_ViewContextTable record and a scratch viewport record.
//
// Calls (original): ViewCtx_GetViewport, Matrix_Inverse4x4, D3DXMatrixMultiply,
// Frustum_Build x2, ViewContext_BuildCascadeFrusta x2, ViewContext_FitShadowCascades
// (reimplemented: reimpl/render/fit_shadow_cascades.hpp),
// thunk_FUN_02537cb0 (the material step), PgPrimitiveScratchRecord_Submit.
// Writes directly: the ctx (ViewContext fields), the g_ViewContextTable record,
// g_CascadeSplitSum, g_ViewContextRecordNext, the scratch record (viewContextIdx,
// techniqueIdx, vertexShader, envIdx, viewId, cmdStream), the scratch command
// buffer (one viewport command) and g_ScratchCmdNext.
//
// VR behavior inside it, as a direct call:
//   - vr/cull_frustum widens the frustum half-extents for HMD views to the
//     eyes' union, right after the game computes them (docs/culling.md).
//   - the shadow cascades are fitted stably (fit_shadow_cascades.hpp,
//     docs/shadows.md).
#pragma once

namespace mc2vr::game {
struct ViewContext;
}

namespace mc2vr::reimpl {

void ViewContext_BuildCameraConstants(game::ViewContext *ctx);

} // namespace mc2vr::reimpl
