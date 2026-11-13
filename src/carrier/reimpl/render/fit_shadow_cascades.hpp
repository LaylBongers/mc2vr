// ViewContext_FitShadowCascades (0x00859ad0) — reimplementation, with the
// cascade fit stabilized (docs/shadows.md).
//
// STATUS: new (2026-11-13), not yet live-verified. The stock path is
// transcribed from the disassembly; a self-check runs the original on a copy
// of the ctx for the first builds and compares every output bit for bit
// (`shadowfit: selfcheck` log line).
// Ghidra plate: ViewContext_FitShadowCascades.
// Original: stdcall(ctx), `ret 4`; called once per view build at the end of
// ViewContext_BuildCameraConstants (0x00859739), whose reimplementation is its
// only caller, so it is called directly and has no entry (the original stays
// callable for the self-check).
//
// The directional light's cascaded shadow maps (four 1024² quarters of the
// 1024x4096 atlas). Original fit:
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
// Stabilized fit (shadow_fit=stable, default) — the same outputs, made
// invariant to camera motion:
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
// The stock path (shadow_fit=stock) is the original, for A/B.
//
// Calls (original): Matrix_LookAtToWorld, Matrix_Copy3x4 (here memcpy),
// Matrix_Inverse4x4, D3DXVec3TransformCoord x40, D3DXMatrixMultiply x9.
// Writes: ctx->cascadeViewProj, cascadeAtlas, cascadeScaleBias, shadowBase.
#pragma once

namespace mc2vr::game {
struct ViewContext;
}

namespace mc2vr::reimpl {

void ViewContext_FitShadowCascades(game::ViewContext *ctx);

// conf `shadow_fit` = stable | stock.
bool set_shadow_fit(const char *value);

// 10 s window report; resets counters.
void shadow_fit_report_window();

} // namespace mc2vr::reimpl
