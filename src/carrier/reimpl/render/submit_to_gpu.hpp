// PgPrimitive_SubmitToGPU (0x00855690) — reimplementation.
//
// STATUS: live; verified against the original (paired passes, docs/reimpl.md).
// Ghidra plate: PgPrimitive_SubmitToGPU.
// Note: docs/reverse_engineering/submit_to_gpu.md.
// Original: void(void), called from RenderShell_RenderFrameTimed (0x0085abd0)
// Ours renders the stereo frame: the original's body runs as two passes,
// LEFT then RIGHT (vr/eye_replay.hpp marks the boundaries), bracketed by one
// OpenXR frame (vr/xr_session.hpp: begin_frame before pass 1, end_frame with
// the eye pair after pass 2).
//
// One frame's GPU submission: BeginSubmit, then the sorted draw-record walk
// (per record: view type, shader constants, pass pick, RT/stencil state,
// command stream, stream/index binding, draw), then EndSubmit and the
// view-type epilogue. Calls: game/render_shell.hpp, game/dx9.hpp,
// game/lti.hpp; the device directly for shader/index/query calls.
//
// VR behavior inside it, as direct calls:
//   - view_rewrite::on_upload_gate at the viewContextData upload (0x00855a78;
//     a MidHook there while the original runs).
//   - the HUD layer (vr/hud.hpp): on headset frames the HUD records leave the
//     walk; pass 1 draws them once into the HUD target, pass 2 skips them, and
//     each pass ends by compositing the layer into its eye image.
//   - world markers (vr/world_markers.hpp): each marker quad is moved and
//     scaled for the pass's eye around its draw.
//   - occlusion queries (vr/occlusion_queries.hpp): the engine's query ring
//     is wrapped in per-eye pairs at frame start; each pass's box draws issue
//     its own eye's query.
//
// Writes directly: g_InRenderFrame, g_NameHash_PgPrimitiveSubmitToGPU, the
// precache globals, g_SuppressPresent, g_CurPassObject, g_LastRecViewFlags,
// DAT_00ce7d24, DAT_01163764/011697c8/0116376c/011697c0/00ff36fc, the
// draw-state cache (game/pg_primitive.hpp), g_CurViewType, g_HalfResPassActive.
#pragma once

namespace mc2vr::reimpl {

void PgPrimitive_SubmitToGPU();

} // namespace mc2vr::reimpl
