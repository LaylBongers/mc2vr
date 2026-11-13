// PgClouds_EmitCloudRender1 / PgClouds_EmitCloudRender2 — see clouds_emit.hpp.
// Follows the originals (0x0047ded0-0x0047e8b0, 0x0047e8c0-0x0047f088) block by
// block in their float operation order (scalar SSE). Deviations: see the
// header; the VR calls are marked "VR:".

#include "reimpl/render/clouds_emit.hpp"

#include <bit>
#include <cstdint>
#include <cstring>

#include "debug/build_census.hpp"
#include "game/bloom.hpp"
#include "game/clouds.hpp"
#include "game/render_shell.hpp"
#include "game/view_context.hpp"
#include "vr/view_rewrite.hpp"

namespace mc2vr::reimpl {

namespace {

using namespace game;

// The originals' constants (.rdata).
constexpr float kOne = 1.0f;                                       // [0x00b9b664]
constexpr float kTwo = 2.0f;                                       // [0x00b92874]
constexpr float kThree = 3.0f;                                     // [0x00ba8990]
constexpr float kMinusTwo = -2.0f;                                 // [0x00bea794]
constexpr float kHalf = 0.5f;                                      // [0x00bbb98c]
constexpr float kMilli = 0.001f;                                   // [0x00b929d8]
constexpr float kNegZero = -0.0f;                                  // [0x00bea764]
constexpr float kSkyDepthA = std::bit_cast<float>(0x3f800010u);    // [0x00beb128] 1.0000019
constexpr float kSkyDepthB = std::bit_cast<float>(0xbf800010u);    // [0x00beb124] -1.0000019

// The return addresses of the originals' Submit calls: what the build census
// files these records under (debug_gui_census=clouds).
constexpr uint32_t kRetRender1Record = 0x0047e7dcu;
constexpr uint32_t kRetRender1Rebind = 0x0047e8a5u;
constexpr uint32_t kRetRender2Record = 0x0047f073u;

constexpr uintptr_t kConstBlockBytes = 0xa0;  // 10 vec4
constexpr uintptr_t kSkyOffset = 0x140;
constexpr uint32_t kAllocBytes = 0x180;

template <class T>
T &at(uint8_t *base, uintptr_t off)
{
    return *reinterpret_cast<T *>(base + off);
}

// A 4-byte copy (the originals' fld/fstp and movq copies).
void copy4(void *dst, const void *src) { std::memcpy(dst, src, 4); }

// One record's command stream in the scratch command buffer.
class Stream {
public:
    Stream() : base_(g_ScratchCmdBase::get()), start_(g_ScratchCmdNext::get()), n_(start_) {}

    void op(uint32_t v) { base_[n_++] = v; }
    void ptr(const void *p) { op((uint32_t)(uintptr_t)p); }
    void f(float v) { op(std::bit_cast<uint32_t>(v)); }
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
    // ops 2/3 from the 10 cQuad params, one vec4 each from `data` (count
    // clamped to 1; params with no register or no count skipped).
    void constants(uint32_t opcode, const ShaderConstParam (&params)[kCloudsConstCount], const float *data)
    {
        for (uint32_t i = 0; i < kCloudsConstCount; i++, data += 4) {
            const ShaderConstParam &p = params[i];
            if (p.start_reg == 0xffffffffu || p.count == 0) continue;
            op(opcode);
            op(p.start_reg);
            ptr(data);
            op((int32_t)p.count < 1 ? p.count : 1u);
        }
    }
    // op 2: the sky matrix (count clamped to 4; no register check).
    void sky_matrix(const ShaderConstParam &p, const float *m)
    {
        if (p.count == 0) return;
        op(2);
        op(p.start_reg);
        ptr(m);
        op((int32_t)p.count < 4 ? p.count : 4u);
    }
    // End of stream: publishes the used dwords, returns the stream.
    uint32_t *end()
    {
        op(0);
        g_ScratchCmdNext::set(n_);
        return base_ + start_;
    }
    uint32_t *base() const { return base_; }

private:
    uint32_t *base_;
    uint32_t start_, n_;
};

ViewContext *current_view()
{
    uint8_t *const shell = g_RenderShellPtrB::get();
    return reinterpret_cast<ViewContext *>(shell + (uint32_t)at<uint16_t>(shell, kRenderShellSubView) *
                                                       kRenderShellViewStride +
                                           0x10);
}

// The constant block (0x47df31-0x47e347). `inv` = the inverse camera matrix;
// its translation enters multiplied by zero.
void build_constants(uint8_t *obj, const float *inv, float *c)
{
    const auto fld = [obj](uintptr_t off) { return at<float>(obj, off); };
    const float s = fld(kCloudsShape);
    const float aspect = fld(kCloudsAspectNum) / fld(kCloudsAspectDen);
    const float eb = fld(kCloudsExtentB) * aspect;
    const float ea = fld(kCloudsExtentA) * aspect;
    const float s2 = s * s;
    const float m2s = s * kMinusTwo;
    const float c4x = ((kOne - s2) * kThree) / ((s2 + kTwo) * kTwo);
    const float c4y = s2 + kOne;
    const float zero = 0.0f;
    const float t0 = inv[12] * zero, t1 = inv[13] * zero, t2 = inv[14] * zero;
    for (int k = 0; k < 2; k++) {
        c[k * 4 + 0] = t0;
        c[k * 4 + 1] = t1;
        c[k * 4 + 2] = t2;
        c[k * 4 + 3] = zero;
    }
    const float r = fld(kCloudsAspectNum) / fld(kCloudsAspectDen);
    c[8] = r;
    c[9] = zero;
    c[10] = r;
    c[11] = zero;
    copy4(&c[12], obj + kCloudsAspectDen);
    c[13] = fld(kCloudsWidth) * kHalf + fld(kCloudsCenter);
    c[14] = fld(kCloudsCenter) - fld(kCloudsWidth) * kHalf;
    copy4(&c[15], obj + kCloudsWidth);
    c[16] = c4x;
    c[17] = c4y;
    c[18] = m2s;
    copy4(&c[19], obj + kCloudsC4W);
    uint8_t *const sky = at<uint8_t *>(g_GlobalCamCtx::get(), kGlobalCamSky);
    std::memcpy(&c[20], sky + kSkyC5, 16);
    const float k = fld(kCloudsBlend);
    const float om = kOne - k;
    for (int row = 0; row < 2; row++) {
        const float *const col = &at<float>(sky, row == 0 ? kSkyColourA : kSkyColourB);
        float *const out = &c[24 + row * 4];
        out[0] = fld(kCloudsTint) * (k * col[0] + om);
        out[1] = fld(kCloudsTint + 4) * (col[1] * k + om);
        out[2] = fld(kCloudsTint + 8) * (col[2] * k + om);
        out[3] = col[3] * k + om;
    }
    c[32] = kOne / (fld(kCloudsRangeHi) - fld(kCloudsRangeLo));
    c[33] = fld(kCloudsRangeLo) / (fld(kCloudsRangeLo) - fld(kCloudsRangeHi));
    c[34] = fld(kCloudsFadeHi) - fld(kCloudsFadeLo);
    copy4(&c[35], obj + kCloudsFadeLo);
    const float eb2 = eb * eb;
    const float den = eb2 - ea * ea;
    c[36] = eb2 / den;
    c[37] = g_CloudsNegOne::get() / den;
    c[38] = fld(kCloudsScale) * kMilli;
    c[39] = fld(kCloudsWidth) * fld(kCloudsScale) * kMilli;
}

// The sky matrix (0x47e34e-0x47e553): transpose(inverse(camera rotation) * P),
// P = the view's projection from the ctx tans with the sky's depth terms.
void build_sky_matrix(const ViewContext *ctx, const float *camera, float *out)
{
    const float zero = 0.0f;
    float p[16];
    for (float &v : p) v = zero;
    p[0] = kNegZero - kOne / ctx->tanH;
    p[5] = kOne / ctx->tanV;
    p[10] = kSkyDepthA;
    p[11] = kOne;
    p[14] = kSkyDepthB;
    float rot[16], inv[16], prod[16];
    std::memcpy(rot, camera, sizeof rot);
    rot[12] = zero;
    rot[13] = zero;
    rot[14] = zero;
    rot[15] = kOne;
    Matrix_Inverse4x4(rot, inv);
    D3DXMatrixMultiply(prod, inv, p);
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) copy4(&out[r * 4 + c], &prod[c * 4 + r]);
    }
}

// The constant allocation shared by both emitters: the block (+0xa0, copied
// to +0) and the sky matrix (+0x140). Returns the base.
float *build_block(uint8_t *obj, const float *camera, ViewContext *ctx)
{
    float inv[16];
    {
        float tmp[16];
        std::memcpy(tmp, camera, sizeof tmp);
        Matrix_Inverse4x4(tmp, inv);
    }
    const uint32_t off = g_ScratchConstNext::get();
    uint8_t *const base = g_ScratchConstBase::get() + off;
    float *const block = reinterpret_cast<float *>(base + kConstBlockBytes);
    build_constants(obj, inv, block);
    std::memcpy(base, block, kConstBlockBytes);
    g_ScratchConstNext::set(off + kAllocBytes);
    float *const sky = reinterpret_cast<float *>(base + kSkyOffset);
    build_sky_matrix(ctx, camera, sky);
    // VR: the sky matrix and the view it was built for; this pass's eye
    // version replaces it when a record's stream uploads it.
    view_rewrite::on_sky_matrix_built(sky, ctx->viewProj);
    return reinterpret_cast<float *>(base);
}

// The record's draw state and the technique's scratch-record setup before Submit
// (0x47e740-0x47e7b7 / 0x47efd4-0x47f040).
void bind_technique(uint8_t *obj, const CloudRenderVP &tech, uint32_t *stream, uintptr_t material_off)
{
    PgPrimitive &sc = *g_ScratchPrimitive::ptr();
    sc.cmdStream = stream;
    sc.geometryObj = reinterpret_cast<PgGeometry *>(obj + kCloudsGeometry);
    sc.techniqueIdx = tech.base.tableIdx;
    void *streams = obj + kCloudsStreams;
    sc.streamDataList = streams;
    sc.streamCount = 1;
    RenderShell *const shell = g_RenderShellPtr::get();
    RenderShell_Slot17(shell, &tech.base, &streams, 1);
    sc.vertexShader = tech.base.vertexShader;
    thunk_FUN_02537cb0(obj + material_off);  // the material: sets the scratch materialIdx
}

} // namespace

void PgClouds_EmitCloudRender1(uint8_t *obj, const float *camera)
{
    PgClouds_ClampParams(obj);
    ViewContext *const ctx = current_view();
    float *const consts = build_block(obj, camera, ctx);
    const float *const block = consts + kConstBlockBytes / 4;
    const float *const sky = consts + kSkyOffset / 4;
    const CloudRenderVP &tech = *g_PgCloudRenderVP1::ptr();
    PgPrimitive &sc = *g_ScratchPrimitive::ptr();

    // ---- the cloud render 1 record 0x47e4e1-0x47e7dc ----
    sc.viewId = (uint16_t)(sc.viewId + 1);
    Stream s;
    s.constants(2, tech.cQuad, block);
    s.sky_matrix(tech.viewProjMtx, sky);
    s.constants(3, g_PgCloudRenderFP1::ptr()->cQuad, consts);
    int32_t x, y, w, h;
    if (ctx->vpUnscaled == 0) {
        const float scale = at<float>(g_RenderShellPtrB::get(), kShellRenderScale);
        x = (int32_t)((float)ctx->vpX * scale);
        y = (int32_t)((float)ctx->vpY * scale);
        w = (int32_t)((float)ctx->vpW * scale);
        h = (int32_t)((float)ctx->vpH * scale);
    } else {
        x = ctx->vpX;
        y = ctx->vpY;
        w = ctx->vpW;
        h = ctx->vpH;
    }
    uint8_t *const sky_params = at<uint8_t *>(g_GlobalCamCtx::get(), kGlobalCamSky);
    if (sky_params[0] == 0) {
        s.op(6);  // the half-size clouds target, no depth
        s.ptr(at<void *>(obj, kCloudsOutRT));
        s.ptr(nullptr);
        s.viewport(x / 2, y / 2, w / 2, h / 2);
        s.op(15);  // clear
        s.op(0);
        s.op(1);
    }
    uint32_t *const stream = s.end();
    bind_technique(obj, tech, stream, kCloudsMaterialRender1);
    const uint16_t material = sc.materialIdx;
    build_census::set_emitter_override(kRetRender1Record);
    PgPrimitiveScratchRecord_Submit(0xfe0, 0, 4, 0x800, 0, 0x801, 0, material);

    // ---- back to main + scene depth, full viewport 0x47e7dc-0x47e8a5 ----
    sc.vertexShader = nullptr;
    sc.envIdx = 0;
    uint8_t *const front = reinterpret_cast<uint8_t *>(g_RenderShellPtr::get());
    Stream r;
    r.op(6);
    r.ptr(at<void *>(front, kRenderShellRT0Surface));
    r.ptr(at<void *>(front, kRenderShellDepthSurface));
    sc.viewId = (uint16_t)(sc.viewId + 1);
    r.viewport(x, y, w, h);
    sc.cmdStream = r.end();
    sc.techniqueIdx = 0x100;
    // The original pushes ESI, the command buffer base with SI = the material.
    const uint32_t last = ((uint32_t)(uintptr_t)s.base() & 0xffff0000u) | material;
    build_census::set_emitter_override(kRetRender1Rebind);
    PgPrimitiveScratchRecord_Submit(0, 0, 0xffff, 0, 0, 0, 0, last);
    build_census::set_emitter_override(0);
}

void PgClouds_EmitCloudRender2(uint8_t *obj, const float *camera)
{
    PgClouds_ClampParams(obj);
    ViewContext *const ctx = current_view();
    float *const consts = build_block(obj, camera, ctx);
    const CloudRenderVP &tech = *g_PgCloudRenderVP2::ptr();

    // ---- the cloud render 2 record 0x47eece-0x47f073 ----
    Stream s;
    s.constants(2, tech.cQuad, consts + kConstBlockBytes / 4);
    s.sky_matrix(tech.viewProjMtx, consts + kSkyOffset / 4);
    uint32_t *const stream = s.end();
    bind_technique(obj, tech, stream, kCloudsMaterialRender2);
    const uint16_t material = (uint16_t)(g_CloudRender2MaterialCounter::get() + 1);
    g_CloudRender2MaterialCounter::set(material);
    build_census::set_emitter_override(kRetRender2Record);
    PgPrimitiveScratchRecord_Submit(0xfe0, 0, 4, 0x800, 0, 0x801, 0, material);
    build_census::set_emitter_override(0);
    g_ScratchPrimitive::ptr()->cmdStream = nullptr;
}

} // namespace mc2vr::reimpl
