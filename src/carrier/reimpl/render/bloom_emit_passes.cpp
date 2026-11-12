// PgLtiBloom_EmitPasses — see bloom_emit_passes.hpp. Follows the original
// (0x0074fae0-0x00750b76) record by record, in its float operation order
// (scalar SSE). Deviations: the fld/fstp float copies are plain copies
// (bit-identical except that x87 would quiet a signaling NaN); pow is the
// C runtime's, not the game's __libm_sse2_pow (may differ in the last bit of
// the double before its float conversion); and the VR changes to the
// composite (motion blur and depth of field off), marked "VR:".

#include "reimpl/render/bloom_emit_passes.hpp"

#include <xmmintrin.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "game/addresses.h"
#include "game/bloom.hpp"
#include "game/render_shell.hpp"
#include "game/view_context.hpp"

namespace mc2vr::reimpl {

namespace {

using namespace game;

// The original's constants (.rdata).
constexpr float kOne = 1.0f;              // [0x00b9b664]
constexpr float kHalf = 0.5f;             // [0x00bbb98c]
constexpr float kOneAndHalf = 1.5f;       // [0x00b9c650]
constexpr float kRadius0 = 0.4f;          // [0x00ba8724]
constexpr float kRadius1 = 0.8f;          // [0x00b9852c]
constexpr float kRadius2 = 0.45f;         // [0x00beb19c]
constexpr float kDtMin = 0.008333334f;    // [0x00beb198]
constexpr float kDtMax = 0.1f;            // [0x00b92b58]
constexpr float kNegZero = -0.0f;         // [0x00bea764]
constexpr float kSineSteps = 8192.0f;     // [0x00bea6e0]
constexpr uint32_t kLcgMul = 0x19660d, kLcgAdd = 0x3c6ef35f;

template <class T>
T &at(uint8_t *base, uintptr_t off)
{
    return *reinterpret_cast<T *>(base + off);
}

// One record's command stream in the scratch command buffer.
class Stream {
public:
    Stream() : base_(g_ScratchCmdBase::get()), start_(g_ScratchCmdNext::get()), n_(start_) {}

    void op(uint32_t v) { base_[n_++] = v; }
    void ptr(const void *p) { op((uint32_t)(uintptr_t)p); }
    void f(float v) { op(std::bit_cast<uint32_t>(v)); }

    // op 6: RT0 (and depth; null = none).
    void target(void *color, void *depth)
    {
        op(6);
        ptr(color);
        ptr(depth);
    }
    // op 8: the viewport, z 0..1.
    void viewport(int32_t x, int32_t y, int32_t w, int32_t h)
    {
        op(8);
        op((uint32_t)x);
        op((uint32_t)y);
        op((uint32_t)w);
        op((uint32_t)h);
        f(0.0f);
        f(kOne);
    }
    // op 3: one vec4 (count clamped to 1; nothing when the shader lacks it).
    void ps_constant(const ShaderConstParam *p, const float *data)
    {
        if (p->count == 0) return;
        const int32_t c = (int32_t)p->count;
        op(3);
        op(p->start_reg);
        ptr(data);
        op((uint32_t)(c < 1 ? c : 1));
    }
    // End of stream: publishes the used dwords, returns the stream.
    uint32_t *end()
    {
        op(0);
        g_ScratchCmdNext::set(n_);
        return base_ + start_;
    }

private:
    uint32_t *base_;
    uint32_t start_, n_;
};

// vec4 constants from the scratch constant buffer.
float *alloc_constants(uint32_t vec4s)
{
    const uint32_t off = g_ScratchConstNext::get();
    g_ScratchConstNext::set(off + vec4s * 16);
    return reinterpret_cast<float *>(g_ScratchConstBase::get() + off);
}

// Submits `stream` as the next scratch record, a w x h screen quad.
void emit(uint32_t *stream, int32_t w, int32_t h, uintptr_t technique, uint8_t *material)
{
    PgPrimitive &scratch = *g_ScratchPrimitive::ptr();
    scratch.viewId = (uint16_t)(scratch.viewId + 1);
    scratch.cmdStream = stream;
    RenderShell_EmitScreenQuad(w, h, technique, material, 0xffffffffu);
}

void *surface(uint8_t *bloom, uintptr_t off) { return at<void *>(bloom, off); }

// A uniform pseudo-random float in [1, 2) from two LCG steps (the original's
// mantissa construction).
float rng_unit(uint32_t lo_step, uint32_t hi_step)
{
    return std::bit_cast<float>((((hi_step & 0xffff01ffu) | (lo_step >> 16)) >> 9) | 0x3f800000u);
}

} // namespace

void PgLtiBloom_EmitPasses(uint8_t *bloom)
{
    uint8_t *const shell = g_RenderShellPtrB::get();
    if (at<uint16_t>(shell, kRenderShellSubViewCount) > 2) {
        return;
    }
    ViewContext *const ctx = reinterpret_cast<ViewContext *>(
        shell + (uint32_t)at<uint16_t>(shell, kRenderShellSubView) * kRenderShellViewStride + 0x10);
    int32_t vx, vy, vw, vh;
    ViewCtx_GetViewport(ctx, &vx, &vy, &vw, &vh);

    // ---- bloom pyramid 0x74fb35-0x74fea6 ----
    const float radius = at<float>(bloom, kBloomRadius);
    const float radii[4] = {radius * kRadius0, radius * kRadius1, kRadius2, g_Const_0_95::get()};
    constexpr int32_t kDivisor[4] = {2, 4, 8, 16};
    constexpr int32_t kBlurPairs[4] = {0, 1, 2, 4};
    for (int level = 0; level < 4; level++) {
        const int32_t w = vw / kDivisor[level];
        const int32_t h = vh / kDivisor[level];
        uint8_t *const mats = bloom + level * kBloomMatStride;
        {
            Stream s;
            if (level == 0) {
                s.op(12);  // main -> ColorBufferCopy
            }
            s.target(surface(bloom, kBloomDownsampleRT + level * 4), nullptr);
            s.viewport(0, 0, w, h);
            emit(s.end(), w, h, (uintptr_t)g_PostFxQuadTechnique::ptr(), mats + kBloomDownsampleMat);
        }
        float *const c = alloc_constants(1);
        const float inv_w = kOne / (float)w;
        const float inv_h = kOne / (float)h;
        c[0] = inv_w * radii[level];
        c[1] = inv_h * radii[level];
        c[2] = inv_w;
        c[3] = inv_h;
        for (int k = 0; k < kBlurPairs[level]; k++) {
            {
                Stream s;
                s.target(surface(bloom, kBloomBlurRT + level * 4), nullptr);
                s.ps_constant(g_BlurPassA_BlurScale::ptr(), c);
                emit(s.end(), w, h, (uintptr_t)g_PostFxQuadTechnique::ptr(), mats + kBloomBlurAMat);
            }
            {
                Stream s;
                s.target(surface(bloom, kBloomDownsampleRT + level * 4), nullptr);
                s.ps_constant(g_BlurPassB_BlurScale::ptr(), c);
                emit(s.end(), w, h, (uintptr_t)g_PostFxQuadTechnique::ptr(), mats + kBloomBlurBMat);
            }
        }
    }

    // ---- flares, luminance 64x64 0x74fea6-0x74ffbb ----
    if ((DAT_019c5144::get() & 1) == 0) {
        DAT_019c5144::set(DAT_019c5144::get() | 1);
        DAT_01994bd0::set(0);
        std::memset(DAT_019947d0::data(), 0, DAT_019947d0::count * sizeof(uint32_t));
    }
    const int32_t flares = FUN_0074efe0(DAT_019947d0::data(), DAT_01994bd0::get());
    {
        Stream s;
        if (flares > 0) {
            s.op(12);  // main (with the flares) -> ColorBufferCopy
        }
        s.target(surface(bloom, kBloomLum64RT), nullptr);
        s.viewport(0, 0, 64, 64);
        emit(s.end(), 64, 64, (uintptr_t)g_PostFxQuadTechnique::ptr(), bloom + kBloomLum64Mat);
    }

    // ---- luminance 16x16, 4x4, 1x1 0x74ffbb-0x750243 ----
    int32_t size = 16;
    for (int k = 0; k < 2; k++) {
        float *const c = alloc_constants(1);  // .zw unwritten
        c[0] = kOne / (float)(size * 4);
        c[1] = kOne / (float)(size * 4);
        Stream s;
        s.target(surface(bloom, kBloomLumRT + k * 4), nullptr);
        s.viewport(0, 0, size, size);
        s.ps_constant(g_LumPass_TexSize::ptr(), c);
        emit(s.end(), size, size, (uintptr_t)g_PostFxQuadTechnique::ptr(), bloom + kBloomLumMat + k * kBloomMatStride);
        size >>= 2;
    }
    {
        float *const c = alloc_constants(1);
        c[0] = kOne / (float)(size * 4);
        c[1] = kOne / (float)(size * 4);
        Stream s;
        s.target(surface(bloom, kBloomLum1RT), nullptr);
        s.viewport(0, 0, size, size);
        s.ps_constant(g_LumFinalPass_TexSize::ptr(), c);
        emit(s.end(), size, size, (uintptr_t)g_PostFxQuadTechnique::ptr(), bloom + kBloomLum1Mat);
    }

    // ---- eye adaptation 0x750243-0x750427 ----
    {
        float *const rate = alloc_constants(1);
        const float g_dt = *reinterpret_cast<const volatile float *>(MC2_G_DT);
        float dt = kDtMin;
        if (!(kDtMin > g_dt)) {  // comiss/ja: NaN keeps g_Dt
            dt = g_dt > kDtMax ? kDtMax : g_dt;
        }
        rate[0] = dt;
        rate[1] = at<float>(bloom, kBloomAdaptRate);
        rate[2] = at<float>(bloom, kBloomAdaptRate + 4);
        rate[3] = (float)std::pow((double)at<float>(bloom, kBloomAdaptRate),
                                  (double)(at<float>(bloom, kBloomAdaptRate + 4) * dt));
        float *const params = alloc_constants(1);
        std::memcpy(params, bloom + kBloomAdaptParams, 16);
        Stream s;
        s.op(22);  // StretchRect: this frame's result -> previous
        s.ptr(surface(bloom, kBloomAdaptRT));
        s.ptr(surface(bloom, kBloomAdaptPrevRT));
        s.target(surface(bloom, kBloomAdaptRT), nullptr);
        s.viewport(0, 0, size, size);
        s.ps_constant(g_AdaptiveLum_AdaptiveParams::ptr(), rate);
        s.ps_constant(g_AdaptiveLum_MinmaxAdaptiveParams::ptr(), params);
        emit(s.end(), size, size, (uintptr_t)g_PostFxQuadTechnique::ptr(), bloom + kBloomAdaptMat);
    }

    // ---- half-size pass 0x750427-0x7504dc ----
    {
        Stream s;
        s.target(surface(bloom, kBloomDofRT), nullptr);
        s.viewport(0, 0, vw / 2, vh / 2);
        emit(s.end(), vw / 2, vh / 2, (uintptr_t)g_PostFxQuadTechnique::ptr(), bloom + kBloomDofMat);
    }

    // ---- composite constants 0x7504dc-0x7505f6 ----
    float *const k = alloc_constants(4);
    float *const image = k, *const filter = k + 4, *const project = k + 8, *const dof = k + 12;
    image[0] = at<float>(bloom, kBloomImageParams);
    image[1] = at<float>(bloom, kBloomImageParams + 4);
    {
        const float sat = at<float>(shell, kRenderShellSaturation);
        image[2] = (kOne - sat) * kHalf + sat * kOneAndHalf;  // .w unwritten
    }
    filter[0] = at<float>(bloom, kBloomFilterParams);
    filter[1] = at<float>(bloom, kBloomFilterParams + 4);
    filter[2] = at<float>(bloom, kBloomFilterParams + 8);
    project[0] = ctx->proj[10];  // .zw unwritten
    project[1] = ctx->proj[14];
    {
        uint8_t *const cam = reinterpret_cast<uint8_t *>(ctx->camera);
        const uint8_t *const e = cam + *ctx->camera * kCamEntryStride;
        std::memcpy(dof, e + kCamEntryDof, 16);
        // The original: filter[3] = 1 - the entry's DOF strength (+0x6c), the
        // floor of the shader's sharp-image weight max(filterParams.w, focus).
        // VR: 1 = always the sharp image: no depth of field.
        filter[3] = kOne;
        dof[0] = kOne / (dof[1] - dof[0]);
        dof[2] = kOne / (dof[2] - dof[3]);
    }

    // ---- film grain 0x7505f6-0x7507bf ----
    {
        const uint32_t r1 = g_PostFxGrainRng::get() * kLcgMul + kLcgAdd;
        const uint32_t r2 = r1 * kLcgMul + kLcgAdd;
        const uint32_t r3 = r2 * kLcgMul + kLcgAdd;
        const uint32_t r4 = r3 * kLcgMul + kLcgAdd;
        g_PostFxGrainRng::set(r4);
        float *const offset = &at<float>(bloom, kBloomGrainOffset);
        offset[0] = rng_unit(r3, r4) - kOne;
        offset[1] = rng_unit(r1, r2) - kOne;
        const float *const scale = &at<float>(bloom, kBloomGrainScale);
        const float steps = kSineSteps / g_TwoPi::get() * at<float>(bloom, kBloomGrainAngle) + kHalf;
        const uint32_t idx = (uint32_t)_mm_cvtt_ss2si(_mm_set_ss(steps));
        const float sn = g_SineTable::at(idx & 0x1fff);
        const float neg_cs = kNegZero - g_SineTable::at((idx - 0x800) & 0x1fff);
        float *const row0 = &at<float>(bloom, kBloomGrainRow0);
        float *const row1 = &at<float>(bloom, kBloomGrainRow1);
        const float u = offset[0] + kHalf, v = offset[1] + kHalf;
        row0[0] = neg_cs * scale[0];
        row0[1] = sn * scale[1];
        row0[3] = kNegZero - ((u * row0[0] + v * row0[1]) - kHalf);
        row1[0] = kNegZero - sn * scale[0];
        row1[1] = neg_cs * scale[1];
        row1[3] = kNegZero - ((u * row1[0] + v * row1[1]) - kHalf);
    }

    // ---- the composite 0x7507bf-0x750b66 ----
    uint8_t *const front_shell = reinterpret_cast<uint8_t *>(g_RenderShellPtr::get());
    Stream s;
    s.target(at<void *>(front_shell, kRenderShellFront), at<void *>(front_shell, kRenderShellAutoDepth));
    // The original: PostFx_BuildMotionBlurMatrix(&m, ctx), the current -> previous frame
    // reprojection (row vectors); X, Y, W = its columns 0, 1, 3, and the
    // velocity scale = (bloom+0x34, 0, 0, 0).
    // VR: no motion blur. Zero rows and scale: zero velocity, so the shader's
    // four taps all read the pixel itself.
    float *const motion = alloc_constants(4);
    std::memset(motion, 0, 4 * 16);
    s.viewport(vx, vy, vw, vh);
    s.ps_constant(g_CompositeImageParams::ptr(), image);
    s.ps_constant(g_CompositeFilterParams::ptr(), filter);
    s.ps_constant(g_CompositeProjectParams::ptr(), project);
    s.ps_constant(g_CompositeDofParams::ptr(), dof);
    s.ps_constant(g_CompositeMotionBlurX::ptr(), motion);
    s.ps_constant(g_CompositeMotionBlurY::ptr(), motion + 4);
    s.ps_constant(g_CompositeMotionBlurW::ptr(), motion + 8);
    s.ps_constant(g_CompositeMotionBlurScale::ptr(), motion + 12);
    emit(s.end(), vw, vh, (uintptr_t)g_PostFxCompositeTechnique::ptr(), bloom + kBloomCompositeMat);
    PgPrimitive &scratch = *g_ScratchPrimitive::ptr();
    scratch.viewId = (uint16_t)(scratch.viewId + 1);
    scratch.cmdStream = nullptr;
}

} // namespace mc2vr::reimpl
