// Sky_EmitRecords — see sky_emit.hpp. Follows the original
// (0x0047f890-0x00480163) block by block in its float operation order
// (scalar SSE). The fix is marked "Fix:".

#include "reimpl/render/sky_emit.hpp"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "debug/build_census.hpp"
#include "game/bloom.hpp"
#include "game/clouds.hpp"
#include "game/render_shell.hpp"
#include "game/sky.hpp"
#include "game/view_context.hpp"
#include "log.hpp"
#include "reimpl/render/clouds_emit.hpp"
#include "reimpl/render/scratch_stream.hpp"

namespace mc2vr::reimpl {

namespace {

using namespace game;

constexpr float kOne = 1.0f;       // [0x00b9b664]
constexpr float kNegZero = -0.0f;  // [0x00bea764]

// The return addresses of the original's Submit calls: what the build census
// files these records under (debug_gui_census=clouds).
constexpr uint32_t kRetWindow = 0x0047fa39u;
constexpr uint32_t kRetDome = 0x0047fd81u;
constexpr uint32_t kRetSun = 0x00480074u;
constexpr uint32_t kRetRestore = 0x0048014cu;

constexpr uint16_t kNoTechnique = 0x100;
constexpr uint8_t kSkyBucket = 2;

std::atomic<uint32_t> g_emits{0}, g_clouds_direct{0}, g_clouds_virtual{0}, g_sky_flag{0};

template <class T>
T &at(uint8_t *base, uintptr_t off)
{
    return *reinterpret_cast<T *>(base + off);
}

// A fresh instance matrix (0x47fa39-0x47fac4 / 0x47fd81-0x47fe00): identity,
// its index in the record's envIdx. Returns it.
float *new_instance_matrix(uint16_t &idx)
{
    idx = g_InstanceMatrixCount::get();
    g_InstanceMatrixCount::set((uint16_t)(idx + 1));
    float *const m = reinterpret_cast<float *>(
        g_PerViewInstanceMatrices + (g_PrimBufferIndex::get() * kInstanceMatricesPerBuffer + idx) * 0x40u);
    for (int k = 0; k < 16; k++) m[k] = (k % 5 == 0) ? kOne : 0.0f;
    return m;
}

// The dome mesh at the current LOD into the scratch record, and the
// technique's setup (0x47faf1-0x47fb35 / 0x47fec7-0x47ff22).
void bind_dome_mesh(uint8_t *sky, const Technique &tech)
{
    PgPrimitive &sc = *g_ScratchPrimitive::ptr();
    const uint32_t lod = g_SkyDomeLod::get();
    sc.geometryObj = at<PgGeometry *>(sky, kSkyGeometry + lod * 4);
    void *streams = at<void *>(sky, kSkyStreams + lod * 4);
    sc.streamDataList = streams;
    sc.techniqueIdx = tech.tableIdx;
    sc.streamCount = 1;
    RenderShell_Slot17(g_RenderShellPtr::get(), &tech, &streams, 1);
    sc.vertexShader = tech.vertexShader;
}

float *alloc_vec4()
{
    const uint32_t off = g_ScratchConstNext::get();
    g_ScratchConstNext::set(off + 0x10);
    return reinterpret_cast<float *>(g_ScratchConstBase::get() + off);
}

void submit(uint32_t ret, uint32_t count, uint32_t first, uint32_t p1, uint32_t p2, uint32_t p3, uint32_t p4,
            uint32_t p5)
{
    const uint16_t seq = (uint16_t)(g_PrimSortSequence::get() + 1);
    g_PrimSortSequence::set(seq);
    build_census::set_emitter_override(ret);
    PgPrimitiveScratchRecord_Submit(count, first, p1, p2, p3, p4, p5, seq);
    build_census::set_emitter_override(0);
}

} // namespace

void Sky_EmitRecords(uint8_t *sky, const float *camera)
{
    PgPrimitive &sc = *g_ScratchPrimitive::ptr();
    uint8_t *const shell = g_RenderShellPtrB::get();
    ViewContext *const ctx = reinterpret_cast<ViewContext *>(
        shell + (uint32_t)at<uint16_t>(shell, kRenderShellSubView) * kRenderShellViewStride + 0x10);
    const float *const sun = reinterpret_cast<const float *>(at<uint8_t *>(g_GlobalCamCtx::get(), kGlobalCamSky) +
                                                             kSkySunDirection);

    // ---- the depth window and the haze range 0x47f8a0-0x47f9bd ----
    SkyDepthWindow win{};
    ViewCtx_GetViewport(ctx, &win.x, &win.y, &win.w, &win.h);
    if (*at<uint8_t *>(shell, kRenderShellReflectionFlag) != 0) {
        uint8_t *const water = g_PgWater::get();
        win.x = 0;
        win.y = 0;
        win.w = at<int32_t>(water, kWaterReflectionW);
        win.h = at<int32_t>(water, kWaterReflectionH);
    }
    const uint8_t *const entry =
        reinterpret_cast<const uint8_t *>(ctx->camera) + ctx->camera[0] * kCamEntryStride;
    const float far_z = *reinterpret_cast<const float *>(entry + kCamEntryFar);
    const float haze_start = far_z * kSkyFarFraction;
    const float haze_end = far_z * kSkyHazeEndFraction;
    win.z = (ctx->proj[10] * haze_start + ctx->proj[14]) / (ctx->proj[11] * haze_start + ctx->proj[15]);

    // ---- A: the window 0x47f952-0x47fa39 ----
    sc.vertexShader = nullptr;
    sc.envIdx = 0;
    sc.viewIdx = 1;
    sc.r34 = 1;  // the depth key
    {
        ScratchStream s;
        s.viewport(win.x, win.y, win.w, win.h, win.z, win.z);
        s.op(11);  // clip planes off
        sc.cmdStream = s.end();
    }
    g_ScratchBucketOverride::set(1);
    sc.techniqueIdx = kNoTechnique;
    sc.rtWriteMask = kSkyBucket;
    thunk_FUN_02537cb0(sky + kSkyMaterialWindow);
    submit(kRetWindow, 0, 0, 0xffff, 0, 0, 0, 0);

    // ---- B: the dome 0x47fa39-0x47fd81 ----
    {
        uint16_t idx;
        float *const m = new_instance_matrix(idx);
        std::memcpy(&m[12], &camera[12], 12);
        sc.envIdx = idx;
    }
    bind_dome_mesh(sky, g_PgSkyVP::ptr()->base);
    float *const scale = alloc_vec4();
    scale[1] = kSkyDomeScaleY;
    scale[0] = kSkyDomeScaleXZ;
    scale[2] = kSkyDomeScaleXZ;
    scale[3] = kOne;
    const bool haze = at<uint8_t *>(g_GlobalCamCtx::get(), kGlobalCamSky)[0] == 0;
    {
        ScratchStream s;
        // Fix: the dome carries the window itself.
        s.viewport(win.x, win.y, win.w, win.h, win.z, win.z);
        if (haze) {
            float *const pp = alloc_vec4();
            std::memcpy(&pp[0], &ctx->proj[10], 4);
            std::memcpy(&pp[1], &ctx->proj[14], 4);
            pp[2] = haze_start;
            pp[3] = kOne / (haze_end - haze_start);
            s.upload(2, g_PgSkyVP::ptr()->scale, scale, 1);
            s.upload(3, g_PgSkyFP::ptr()->projectParams, pp, 1);
            s.op(12);  // main -> ColorBufferCopy
            s.op(13);  // RenderCmd_StretchRectFromGlobal(0)
            s.op(0);
        } else {
            s.upload(2, g_PgSkyVP::ptr()->scale, scale, 1);
        }
        sc.cmdStream = s.end();
    }
    thunk_FUN_02537cb0(sky + (haze ? kSkyMaterialDomeHaze : kSkyMaterialDome));
    const int32_t segments = g_SkyDomeSegments::data()[g_SkyDomeLod::get()];
    const uint32_t verts = (uint32_t)((segments + 1) * (segments + 1));
    const uint32_t strip = (uint32_t)((segments + 2) * segments * 2 - 2);
    submit(kRetDome, strip - 2, 0, 5, verts, 0, verts, 0);

    // ---- C: the sun 0x47fd81-0x480074 ----
    {
        uint16_t idx;
        float *const m = new_instance_matrix(idx);
        m[12] = sun[0] * kSunOffset + camera[12];
        m[13] = camera[13] + sun[1] * kSunOffset;
        m[14] = camera[14] + sun[2] * kSunOffset;
        const float dir[4] = {kNegZero - sun[0], kNegZero - sun[1], kNegZero - sun[2], kNegZero - sun[3]};
        Matrix_OrientFromDirection(dir, m);
        float s[16];
        D3DXMatrixScaling(s, kSunQuadScale, kSunQuadScale, kSunQuadScale);
        D3DXMatrixMultiply(m, s, m);
        sc.envIdx = idx;
    }
    bind_dome_mesh(sky, g_PgSunVP::ptr()->base);
    float *const pos = alloc_vec4();
    pos[0] = camera[12] + sun[0] * kSunDistance * kSunOffset;
    pos[1] = camera[13] + sun[1] * kSunDistance * kSunOffset;
    pos[2] = camera[14] + sun[2] * kSunDistance * kSunOffset;
    pos[3] = 0.0f;
    {
        ScratchStream s;
        // Fix: the sun carries the window itself.
        s.viewport(win.x, win.y, win.w, win.h, win.z, win.z);
        s.upload(2, g_PgSunVP::ptr()->sunPosition, pos, 1);
        sc.cmdStream = s.end();
    }
    thunk_FUN_02537cb0(sky + kSkyMaterialSun);
    submit(kRetSun, 0x3e, verts, 5, verts + 0x40, strip, 0x40, 0);

    // ---- cloud render 2 0x480074-0x480096 ----
    if (at<uint8_t *>(g_GlobalCamCtx::get(), kGlobalCamSky)[0] == 0) {
        uint8_t *const clouds = at<uint8_t *>(sky, kSkyClouds);
        if (*reinterpret_cast<uintptr_t *>(clouds) == kPgCloudsVtable) {
            // Fix: called directly with the window (the original's slot 2
            // tail-jumps to slot 6, PgClouds_EmitCloudRender2).
            PgClouds_EmitCloudRender2InWindow(clouds, camera, win);
            g_clouds_direct.fetch_add(1, std::memory_order_relaxed);
        } else {
            PgClouds_Slot2(clouds, camera);
            g_clouds_virtual.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        g_sky_flag.fetch_add(1, std::memory_order_relaxed);
    }

    // ---- D: the restore 0x48009c-0x48014c ----
    {
        ScratchStream s;
        s.viewport(win.x, win.y, win.w, win.h);
        s.op(10);  // clip planes back on
        sc.cmdStream = s.end();
    }
    sc.techniqueIdx = kNoTechnique;
    sc.vertexShader = nullptr;
    sc.envIdx = 0;
    thunk_FUN_02537cb0(sky + kSkyMaterialRestore);
    {
        const uint16_t seq = (uint16_t)(g_PrimSortSequence::get() + 1);
        g_PrimSortSequence::set(seq);
        sc.rtWriteMask = 0;
        build_census::set_emitter_override(kRetRestore);
        PgPrimitiveScratchRecord_Submit(0, 0, 0xffff, 0, 0, 0, 0, seq);
        build_census::set_emitter_override(0);
    }
    sc.cmdStream = nullptr;
    g_ScratchBucketOverride::set(0);
    g_emits.fetch_add(1, std::memory_order_relaxed);
}

bool anchored_at_camera(const Technique *tech)
{
    return tech != nullptr && (tech == &g_PgSkyVP::ptr()->base || tech == &g_PgSunVP::ptr()->base);
}

void sky_report_window()
{
    const uint32_t emits = g_emits.exchange(0, std::memory_order_relaxed);
    const uint32_t direct = g_clouds_direct.exchange(0, std::memory_order_relaxed);
    const uint32_t virt = g_clouds_virtual.exchange(0, std::memory_order_relaxed);
    const uint32_t flag = g_sky_flag.exchange(0, std::memory_order_relaxed);
    if (emits != 0) {
        MC2VR_LOG("sky: window: emits=%u clouds in window=%u clouds virtual=%u sky flag set=%u", emits, direct, virt,
                  flag);
    }
}

} // namespace mc2vr::reimpl
