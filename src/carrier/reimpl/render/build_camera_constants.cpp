// ViewContext_BuildCameraConstants — see build_camera_constants.hpp. Follows
// the original (0x008591ac-0x00859920) block by block, in its float operation
// order; deviations: the 16-float copies (Matrix_Copy3x4, fld/fstp) and the
// record's fld/fstp copies are memcpy (bit-identical except that x87 would
// quiet a signaling NaN).

#include "reimpl/render/build_camera_constants.hpp"

#include <cstdint>
#include <cstring>

#include "game/render_shell.hpp"
#include "game/view_context.hpp"
#include "reimpl/render/fit_shadow_cascades.hpp"
#include "vr/cull_frustum.hpp"
#include "vr/view_rewrite.hpp"

namespace mc2vr::reimpl {

namespace {

using game::ViewContext;

// The original's constants (.rdata).
constexpr float kHalf = 0.5f;            // [0x00bbb98c]
constexpr float kOne = 1.0f;             // [0x00b9b664]
constexpr float kNegZero = -0.0f;        // [0x00bea764]: `-0.0 - x` negates
constexpr float kFovScale = 0.75f;       // [0x00b9776c]
constexpr float kSineSteps = 8192.0f;    // [0x00bea6e0]
constexpr float kDefaultFar = 200.0f;    // [0x00b984a8]
constexpr float kCascadeRatio = 3.0f;    // [0x00ba8990]
constexpr uint32_t kRe70Camera = 0x3f2c092du;  // [0x00beb188] 0.672015

void copy16(float *dst, const float *src)
{
    std::memcpy(dst, src, 16 * sizeof(float));
}

template <class T>
T shell_field(const uint8_t *shell, uintptr_t off)
{
    T v;
    std::memcpy(&v, shell + off, sizeof v);
    return v;
}

} // namespace

void ViewContext_BuildCameraConstants(game::ViewContext *ctx)
{
    float base = kHalf;  // tan of the half fov ([esp+0x30]); 0.5 without a camera
    ctx->vpUnscaled = 0;
    copy16(ctx->prevViewProj, ctx->viewProj);
    int32_t vx, vy, vw, vh;
    game::ViewCtx_GetViewport(ctx, &vx, &vy, &vw, &vh);

    // ---- viewport aspect relative to the render size 0x8591ed-0x859269 ----
    const uint8_t *const shell = game::g_RenderShellPtrB::get();
    const float scale = shell_field<float>(shell, game::kShellRenderScale);
    const float fw = (float)vw, fh = (float)vh;
    const float sw = (float)(int32_t)((float)shell_field<int32_t>(shell, game::kShellRenderW) * scale);
    const float ratio_w = fw / sw;
    const float sh = (float)(int32_t)((float)shell_field<int32_t>(shell, game::kShellRenderH) * scale);
    const float ratio_h = fh / sh;
    const float ratio = ratio_w / ratio_h;
    const float fov_aspect = shell_field<float>(shell, game::kShellFovAspect) * ratio;  // [esp+0x2c]
    const float tanh_scale = shell_field<float>(shell, game::kShellTanHScale) * ratio;  // [esp+0xc]

    // ---- camera 0x85927e-0x8593bf ----
    uint32_t *const cam = ctx->camera;
    if (cam != nullptr) {
        const uint8_t *const e = reinterpret_cast<const uint8_t *>(cam) + cam[0] * game::kCamEntryStride;
        std::memcpy(&ctx->frustum.nearZ, e + game::kCamEntryNear, 4);
        std::memcpy(&ctx->frustum.farZ, e + game::kCamEntryFar, 4);
        copy16(ctx->view, reinterpret_cast<const float *>(e + game::kCamEntryWorld));
        float fov;
        std::memcpy(&fov, e + game::kCamEntryFov, 4);
        float t = fov * kFovScale;
        t = t * fov_aspect;
        const float steps = kSineSteps / game::g_TwoPi::get();
        t = t * steps;
        t = t + kOne;
        t = t * kHalf;
        const int32_t i = (int32_t)t;
        const float *const sine = game::g_SineTable::data();
        const float cosv = kNegZero - sine[(uint32_t)(i - 0x800) & 0x1fff];
        base = sine[(uint32_t)i & 0x1fff] / cosv;
        std::memcpy(&ctx->re70, &kRe70Camera, 4);
        std::memcpy(&ctx->re74, e + game::kCamEntryE74, 4);
    } else {
        ctx->frustum.nearZ = kOne;
        ctx->frustum.farZ = kDefaultFar;
        for (int k = 0; k < 16; k++) ctx->view[k] = (k % 5 == 0) ? kOne : 0.0f;
        ctx->re70 = kOne;
        ctx->re74 = 0.0f;
    }
    ctx->frustum.invNear = kOne / ctx->frustum.nearZ;
    copy16(ctx->cameraWorld, ctx->view);
    float inv[16];
    game::Matrix_Inverse4x4(ctx->view, inv);
    copy16(ctx->view, inv);

    // ---- frustum half-extents 0x8593e9-0x859436 ----
    float th = fh * fov_aspect;
    th = th / fw;
    const float inv_fa = kOne / fov_aspect;
    th = th * (inv_fa * tanh_scale);
    th = th * base;
    ctx->tanH = th;
    float tv = inv_fa * base;
    ctx->tanV = tv;
    // VR: HMD views cull with the eyes' union frustum; every product below
    // derives from th/tv (docs/culling.md).
    cull_frustum::HmdView hmd;
    const bool hmd_view = cull_frustum::on_tans_ready(ctx, th, tv, &hmd);

    // ---- corner directions and projection 0x85943b-0x859562 ----
    const float c0[3] = {kNegZero - th, tv, kOne};            // [esp+0x10]
    const float c1[3] = {kNegZero - th, kNegZero - tv, kOne}; // [esp+0x54]
    const float c2[3] = {th, kNegZero - tv, kOne};            // [esp+0x48]
    const float c3[3] = {th, tv, kOne};                       // [esp+0x3c]
    const float inv_th = kOne / th, inv_tv = kOne / tv;
    const float q = ctx->frustum.farZ / (ctx->frustum.farZ - ctx->frustum.nearZ);
    float *const p = ctx->proj;
    for (int k = 0; k < 16; k++) p[k] = 0.0f;
    p[0] = kNegZero - inv_th;
    p[5] = inv_tv;
    p[10] = q;
    p[11] = kOne;
    p[14] = kNegZero - ctx->frustum.nearZ * q;
    float vp[16];
    game::D3DXMatrixMultiply(vp, ctx->view, ctx->proj);
    copy16(ctx->viewProj, vp);
    if (hmd_view) {
        // VR: the records copied from this VP carry this HMD sample and union
        // pose; the per-eye rewrite looks them up by the VP (vr/view_rewrite.hpp).
        view_rewrite::on_hmd_view_built(ctx->viewProj, hmd.sample, hmd.rot, hmd.pos);
    }

    // ---- cascade splits: base * 3^i 0x859562-0x8595e7 ----
    float sum = 0.0f;
    for (uint32_t i = 0; i < 4; i++) {
        float x = kCascadeRatio, r = kOne;
        for (uint32_t n = i;;) {
            if (n & 1) r = r * x;
            n >>= 1;
            if (n == 0) break;
            x = x * x;
        }
        const float s = r * shell_field<float>(shell, game::kShellCascadeBase);
        ctx->cascadeSplits[i] = s;
        sum = s + sum;
    }
    {
        float acc = ctx->cascadeSplits[0];
        acc = ctx->cascadeSplits[1] + acc;
        acc = ctx->cascadeSplits[2] + acc;
        acc = ctx->cascadeSplits[3] + acc;
        game::g_CascadeSplitSum::set(acc);
    }

    // ---- frusta 0x8595e9-0x85973e ----
    game::Frustum_Build(&ctx->frustum, ctx->cameraWorld, c0, c1, c2, c3);
    ctx->frustumShadow.invNear = ctx->frustum.invNear;
    ctx->frustumShadow.nearZ = ctx->frustum.nearZ;
    ctx->frustumShadow.farZ = ctx->frustum.nearZ + sum;
    game::Frustum_Build(&ctx->frustumShadow, ctx->cameraWorld, c0, c1, c2, c3);
    ctx->r950 = 0;
    game::ViewContext_BuildCascadeFrusta(game::g_CascadeSplitSum::ptr(), ctx, &ctx->frustum, ctx->whole, ctx->wholeB,
                                         ctx->wholeC, 1, c0, c1, c2, c3);
    ctx->r468 = 0;
    game::ViewContext_BuildCascadeFrusta(ctx->cascadeSplits, ctx, &ctx->frustumShadow, ctx->cascades,
                                         ctx->cascadesB, ctx->cascadesC, 4, c0, c1, c2, c3);
    ctx->ra90 = 0xf;
    ViewContext_FitShadowCascades(ctx);  // reimplemented (stabilized fit)

    // ---- this frame's g_ViewContextTable record 0x85973e-0x859882 ----
    game::PgPrimitive &scratch = *game::g_ScratchPrimitive::ptr();
    const uint8_t idx = game::g_ViewContextRecordNext::get();
    game::g_ViewContextRecordNext::set((uint8_t)(idx + 1));
    const uint32_t buf = game::g_PrimBufferIndex::get();
    game::ViewContextRecord &rec = game::g_ViewContextRecords::at(buf * 32 + idx);
    copy16(rec.viewProj, ctx->viewProj);
    scratch.viewContextIdx = idx;
    scratch.techniqueIdx = 0x100;
    scratch.vertexShader = nullptr;
    scratch.envIdx = 0;
    std::memcpy(rec.cameraPos, ctx->cameraWorld + 12, 3 * sizeof(float));
    rec.cameraPos[3] = kOne;
    rec.atmosphereData = reinterpret_cast<const uint8_t *>(game::g_AtmosphereDataBuffers + buf * 0x700);
    rec.globalLightData = reinterpret_cast<const void *>(game::g_GlobalLightDataBuffers + buf * 0x1b00);

    // ---- scratch viewport record 0x859887-0x859918 ----
    scratch.viewId = (uint16_t)(scratch.viewId + 1);
    uint32_t *const cmds = game::g_ScratchCmdBase::get();
    const uint32_t n = game::g_ScratchCmdNext::get();
    uint32_t *const cmd = cmds + n;
    cmd[0] = 8;  // viewport
    cmd[1] = (uint32_t)vx;
    cmd[2] = (uint32_t)vy;
    cmd[3] = (uint32_t)vw;
    cmd[4] = (uint32_t)vh;
    const float min_z = 0.0f, max_z = kOne;
    std::memcpy(&cmd[5], &min_z, 4);
    std::memcpy(&cmd[6], &max_z, 4);
    cmd[7] = 0;  // end of stream
    game::g_ScratchCmdNext::set(n + 8);
    scratch.cmdStream = cmd;
    uint8_t *const shell_a = reinterpret_cast<uint8_t *>(game::g_RenderShellPtr::get());
    game::thunk_FUN_02537cb0(reinterpret_cast<const void *>(shell_field<uintptr_t>(shell_a, game::kShellVmStepArg)));
    game::PgPrimitiveScratchRecord_Submit(0, 0, 0xffff, 0, 0, 0, 0, scratch.materialIdx);
    scratch.viewId = (uint16_t)(scratch.viewId + 1);
    scratch.cmdStream = nullptr;
}

} // namespace mc2vr::reimpl
