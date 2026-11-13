// PgClouds_EmitCloudRender1 (0x0047ded0) and PgClouds_EmitCloudRender2 (0x0047e8c0)
// — reimplementation.
//
// STATUS: done. Live-verified in gameplay (2026-11-13: per-eye sky matrices,
// no regressions); statically checked against the disassembly instruction by
// instruction (2026-11-13: constants bit-identical, streams, record fields,
// calls and branches match). Not compared against the original call at runtime.
// Ghidra plates: PgClouds_EmitCloudRender1, PgClouds_EmitCloudRender2.
// Note: docs/view_rewrite.md "Clouds".
// Original: thiscall(clouds, camera world matrix), ret 4; slots 3 and 6 of the
// clouds vtable 0x00bab5b0, on the producer side (the scene build), once per
// frame each for the main view.
// Entry: game/clouds.hpp's declarations (hooks/reimpl.cpp).
//
// Both build the same 10-vec4 constant block (cloud shape, colours, ranges;
// written at +0xa0 of a 0x180-byte scratch constant allocation and copied to
// +0) and a camera-relative sky matrix at +0x140, g_ViewProjMtx =
// transpose(inverse(camera rotation) * P), P from the view ctx tans with the
// sky's own depth terms (A' = 1.0000019, B' = -A'). Then:
//   - EmitCloudRender1: one record (VS c0..c9 and the sky matrix, PS c0..c9)
//     drawing the clouds into their half-size target (mCloudOutRT, half the
//     view's viewport; skipped when the sky params' flag is set), and a second,
//     technique-less record re-binding main + scene depth and the full viewport;
//   - EmitCloudRender2: one record (VS c0..c9 and the sky matrix) drawn into
//     whatever is bound (main, scene depth).
//
// Calls (original): PgClouds_ClampParams, Matrix_Inverse4x4 x2,
// D3DXMatrixMultiply, RenderShell vtable slot 17, thunk_FUN_02537cb0,
// PgPrimitiveScratchRecord_Submit (x2 / x1).
// Writes directly: the scratch constant and command buffers and their next
// counters, the scratch record's fields, cloud render 2's material counter.
// Deviations: the fld/fstp float copies (Matrix_Copy, transposes) are plain
// copies (bit-identical except that x87 would quiet a signaling NaN).
//
// VR behavior (docs/view_rewrite.md "Clouds"): each sky matrix is
// registered with the per-eye channel (view_rewrite::on_sky_matrix_built)
// together with the view it was built for; SubmitToGPU's stream uploads then
// carry this pass's eye version. The records themselves are unchanged.
#pragma once

#include <cstdint>

namespace mc2vr::reimpl {

void PgClouds_EmitCloudRender1(uint8_t *clouds, const float *camera);
void PgClouds_EmitCloudRender2(uint8_t *clouds, const float *camera);

} // namespace mc2vr::reimpl
