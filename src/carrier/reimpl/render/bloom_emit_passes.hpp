// PgLtiBloom_EmitPasses (0x0074fae0) — reimplementation.
//
// STATUS: live; verified by eye in gameplay (2026-11-12). Not compared
// against the original call for call.
// Ghidra plate: PgLtiBloom_EmitPasses.
// Note: docs/motion_blur.md.
// Original: thiscall(bloom), no stack arguments, plain ret; slot 1 of the
// bloom vtable 0x00bd4a14, called by PostFx_ChainAndComposite on the producer
// side (the scene build), once per frame for the post chain's sub-view.
// Entry: game/bloom.hpp's declaration (hooks/reimpl.cpp).
//
// Builds the post chain's draw records, each a scratch command stream
// submitted as a screen quad (RenderShell_EmitScreenQuad), in order:
//   - the bloom pyramid: ColorBufferCopy (op 12), 4 downsample levels (/2 .. /16)
//     with 0, 1, 2, 4 two-pass blurs;
//   - the sun/lens flares into main (FUN_0074efe0), then op 12 again;
//   - the luminance chain 64 -> 16 -> 4 -> 1 and the eye adaptation (op 22 keeps
//     the previous frame's result);
//   - a half-size pass (inferred: the DOF blur image);
//   - the composite into the front with the auto depth (shader3.bin 0x24e990):
//     tone mapping, adaptation, bloom, grain, grading, DOF and motion blur.
//
// Calls (original): ViewCtx_GetViewport, RenderShell_EmitScreenQuad x9,
// FUN_0074efe0. (PostFx_BuildMotionBlurMatrix, 0x0046ae70, is no longer
// called.)
// Writes directly: the scratch command buffer and g_ScratchCmdNext, the
// scratch constant buffer and g_ScratchConstNext, the scratch record's viewId
// and cmdStream, the grain fields of the composite material, g_PostFxGrainRng,
// FUN_0074efe0's static buffer (first call).
//
// VR behavior (docs/motion_blur.md): the composite is emitted without motion
// blur (zero reprojection rows and velocity scale) and without depth of field
// (filterParams.w = 1). Always: neither is wanted in a headset.
#pragma once

#include <cstdint>

namespace mc2vr::reimpl {

void PgLtiBloom_EmitPasses(uint8_t *bloom);

} // namespace mc2vr::reimpl
