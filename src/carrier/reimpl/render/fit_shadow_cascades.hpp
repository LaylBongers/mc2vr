// ViewContext_FitShadowCascades (0x00859ad0) — reimplementation, with the
// cascade fit stabilized (docs/shadows.md).
//
// STATUS: live (2026-11-13/14): edges static under head motion and while
// driving. A transcription of the original was verified bit for bit against
// it in game (240 fits, 0 differences) before the stabilized fit replaced it;
// git has it.
// Ghidra plate: ViewContext_FitShadowCascades.
// Original: stdcall(ctx), `ret 4`; called once per view build at the end of
// ViewContext_BuildCameraConstants (0x00859739), whose reimplementation is its
// only caller, so it is called directly and has no entry.
//
// The directional light's cascaded shadow maps (four square quarters of the
// atlas, shadow_resolution² each). Original fit:
//   - light view = inverse(Matrix_LookAtToWorld(eye = camera position,
//     target = camera position + shell sun direction)), up = world Y;
//   - base ortho: the tight light-space AABB of the whole shadow range
//     (ctx->frustumShadow corners), z near pulled back 300 units for casters;
//   - per cascade: the tight x/y AABB of its slice (ctx->cascades) in base
//     NDC -> the render VP, the atlas scale/bias and the world -> atlas matrix.
// The texel grid follows the camera position continuously and the AABB
// resizes with the camera rotation: shadow edges are resampled every frame
// (crawl/flicker), continuously in VR (head tracking).
//
// Ours: the same outputs, made invariant to camera motion:
//   - light view: rotation only (the sun direction, tracked exactly: the
//     environment regions blend it over seconds), its origin snapped to a
//     kOriginQuantum grid near the camera;
//   - per cascade: a bounding sphere of the slice (rotation-invariant radius),
//     the texel size T = kOriginQuantum / N for an integer N (kept while it
//     fits within kRescaleSlack), the cascade centre snapped to whole texels.
//     As every origin step is N whole texels, texel boundaries are fixed in
//     the world;
//   - depth: from a bounding sphere of the whole shadow range (the same
//     300-unit caster pull-back), so its scale doesn't change with rotation.
//
// Calls: Matrix_LookAtToWorld, Matrix_Inverse4x4, D3DXVec3TransformCoord,
// D3DXMatrixMultiply.
// Writes: ctx->cascadeViewProj, cascadeAtlas, cascadeScaleBias, shadowBase.
#pragma once

namespace mc2vr::game {
struct ViewContext;
}

namespace mc2vr::reimpl {

void ViewContext_FitShadowCascades(game::ViewContext *ctx);


// 10 s window report; resets counters.
void shadow_fit_report_window();

} // namespace mc2vr::reimpl
