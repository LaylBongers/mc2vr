// The view render-ctx (a render-shell view block + 0x10) as far as
// ViewContext_BuildCameraConstants writes it, the builder's callees, and the
// globals it touches. Source of truth: the Ghidra plates on
// ViewContext_BuildCameraConstants (0x008591ac), Frustum_Build,
// ViewContext_BuildCascadeFrusta, ViewCtx_GetViewport
// (docs/reverse_engineering/view_frustum.md, draw_camera_chain.md).
// rNNN = role unknown. Change both together.
#pragma once

#include <cstddef>
#include <cstdint>

#include "game/function.hpp"
#include "game/global.hpp"
#include "game/pg_primitive.hpp"

namespace mc2vr::game {

// Frustum_Build's output (0x218 bytes): near, far, 1/near, then the 8
// world-space corners at +0x0c (near 0..3 = (+H,+V)(+H,-V)(-H,-V)(-H,+V),
// then far), side-plane normals at +0x6c, plane D's at +0xb4, ...
struct Frustum {
    float nearZ;              // +0x00
    float farZ;               // +0x04
    float invNear;            // +0x08
    uint8_t r00c[0x218 - 0x0c];
};
static_assert(sizeof(Frustum) == 0x218);

// A camera object (ctx+0x28): self-indexed, active entry = cam + [cam] * 0x70.
// Offsets are from that entry.
constexpr uintptr_t kCamEntryStride = 0x70;
constexpr uintptr_t kCamEntryWorld = 0x10;  // 4x4 rows (rotation + position, w = 1)
constexpr uintptr_t kCamEntryNear = 0x50;
constexpr uintptr_t kCamEntryFar = 0x54;
constexpr uintptr_t kCamEntryFov = 0x58;    // radians, 4:3-normalized
constexpr uintptr_t kCamEntryE74 = 0x74;    // copied to ctx+0xe74 (role unknown)

struct ViewContext {
    uint8_t r000[0x10];
    int32_t vpX, vpY, vpW, vpH;  // +0x10 viewport (ViewCtx_GetViewport)
    uint8_t vpUnscaled;          // +0x20 0 = ViewCtx_GetViewport scales by the render scale
    uint8_t r021[0x28 - 0x21];
    uint32_t *camera;            // +0x28 camera object, null = identity defaults
    uint8_t r02c[0x30 - 0x2c];
    float tanH, tanV;            // +0x30 frustum half-extents (render constants)
    Frustum frustum;             // +0x38 the view
    Frustum frustumShadow;       // +0x250 near .. near + sum of the cascade splits
    uint32_t r468;               // +0x468
    uint8_t cascades[0x8c0 - 0x46c];  // +0x46c four shadow cascades (ViewContext_BuildCascadeFrusta)
    uint8_t cascadesB[0x900 - 0x8c0]; // +0x8c0
    uint8_t cascadesC[0x940 - 0x900]; // +0x900
    float cascadeSplits[4];      // +0x940 split distances, base * 3^i
    uint32_t r950;               // +0x950
    uint8_t whole[0xa70 - 0x954];     // +0x954 whole view (ViewContext_BuildCascadeFrusta, count 1)
    uint8_t wholeB[0xa80 - 0xa70];    // +0xa70
    uint8_t wholeC[0xa90 - 0xa80];    // +0xa80
    uint32_t ra90;               // +0xa90 set to 0xf
    uint8_t ra94[0xaa0 - 0xa94];
    float view[16];              // +0xaa0 world -> view (the camera entry, inverted)
    float cameraWorld[16];       // +0xae0 the camera entry (rows; position at +0x30)
    float proj[16];              // +0xb20
    float viewProj[16];          // +0xb60 view * proj -> the g_ViewContextTable record
    float prevViewProj[16];      // +0xba0 last build's viewProj
    uint8_t rbe0[0xe70 - 0xbe0];
    float re70;                  // +0xe70 0.672015 with a camera, 1.0 without (role unknown)
    float re74;                  // +0xe74 camera entry +0x74 (role unknown)
};
static_assert(offsetof(ViewContext, vpX) == 0x10 && offsetof(ViewContext, vpUnscaled) == 0x20);
static_assert(offsetof(ViewContext, camera) == 0x28 && offsetof(ViewContext, tanH) == 0x30);
static_assert(offsetof(ViewContext, frustum) == 0x38 && offsetof(ViewContext, frustumShadow) == 0x250);
static_assert(offsetof(ViewContext, r468) == 0x468 && offsetof(ViewContext, cascadeSplits) == 0x940);
static_assert(offsetof(ViewContext, whole) == 0x954 && offsetof(ViewContext, ra90) == 0xa90);
static_assert(offsetof(ViewContext, view) == 0xaa0 && offsetof(ViewContext, cameraWorld) == 0xae0);
static_assert(offsetof(ViewContext, proj) == 0xb20 && offsetof(ViewContext, viewProj) == 0xb60);
static_assert(offsetof(ViewContext, prevViewProj) == 0xba0 && offsetof(ViewContext, re70) == 0xe70);
static_assert(sizeof(ViewContext) == 0xe78);

// ---- the render shell's fields the builder reads (g_RenderShellPtrB) ----------------
constexpr uintptr_t kShellRenderW = 0x2b98;       // int
constexpr uintptr_t kShellRenderH = 0x2b9c;       // int
constexpr uintptr_t kShellCascadeBase = 0x2bc0;   // float, first cascade split
constexpr uintptr_t kShellFovAspect = 0x2bc8;     // float, scales the fov by the viewport aspect
constexpr uintptr_t kShellTanHScale = 0x2bcc;     // float, scales tanH
constexpr uintptr_t kShellRenderScale = 0x2bd4;   // float
constexpr uintptr_t kShellVmStepArg = 0x3e8c;     // g_RenderShellPtr: ECX for thunk_FUN_02537cb0

// ---- globals ----------------------------------------------------------------------
using g_SineTable = GlobalArray<float, 0x00cf1900u, 0x2000>;  // sin over one turn, filled at boot
using DAT_00d2ea5c = Global<float, 0x00d2ea5cu>;              // 2*pi (writable data)
using DAT_019c579c = Global<float, 0x019c579cu>;              // the sum of the cascade splits
using DAT_00ed9d42 = Global<uint8_t, 0x00ed9d42u>;            // next g_ViewContextTable record this frame
// Write side of g_ViewContextTable: record (bufIdx * 32 + idx); the read side
// is the g_ViewContextTable pointer.
using g_ViewContextRecords = GlobalArray<ViewContextRecord, 0x018c45e0u, 64>;
constexpr uintptr_t kAtmosphereDataBase = 0x017e41e0u;  // + bufIdx * 0x700 -> record +0x60
constexpr uintptr_t kGlobalLightDataBase = 0x018c0fe0u; // + bufIdx * 0x1b00 -> record +0x64
// The scratch draw record PgPrimitiveScratchRecord_Submit copies into the
// queue (PgPrimitive layout; the primitive parameters are Submit's args).
using g_ScratchPrimitive = Global<PgPrimitive, 0x01163708u>;
// The scratch command buffer: base pointer and next free dword.
using DAT_00ff36c0 = Global<uint32_t *, 0x00ff36c0u>;
using DAT_00ff36d4 = Global<uint32_t, 0x00ff36d4u>;

// ---- callees ------------------------------------------------------------------------
// ViewCtx_GetViewport(ctx, &x, &y, &w (EDX), &h (ECX)), stdcall.
inline constexpr Function<void(ViewContext *ctx, int32_t *x, int32_t *y, int32_t *w, int32_t *h)>
    ViewCtx_GetViewport{"ViewCtx_GetViewport", 0x0085a7e0u, {STK, STK, STK, EDX, ECX}};
inline constexpr Function<void(const float *src, float *dst)> Matrix_Inverse4x4{"Matrix_Inverse4x4", 0x008225c0u,
                                                                                 {ESI, EDI}};
// The game's static D3DX: out = a * b (row vectors), stdcall.
inline constexpr Function<float *(float *out, const float *a, const float *b)> D3DXMatrixMultiply{
    "D3DXMatrixMultiply", 0x0096bd94u, {STK, STK, STK}};
// Frustum_Build(frustum, cameraWorld, the four corner directions), stdcall.
inline constexpr Function<void(Frustum *f, const float *cam, const float *c0, const float *c1, const float *c2,
                               const float *c3)>
    Frustum_Build{"Frustum_Build", 0x00857140u, {STK, STK, STK, STK, STK, STK}};
// ViewContext_BuildCascadeFrusta(splits (EAX, count entries), ctx, frustum,
// outA, outB, outC, count, the four corner directions), stdcall.
inline constexpr Function<void(const float *splits, ViewContext *ctx, const Frustum *f, void *a, void *b, void *c,
                               uint32_t count, const float *c0, const float *c1, const float *c2, const float *c3)>
    ViewContext_BuildCascadeFrusta{"ViewContext_BuildCascadeFrusta",
                                   0x0085a3f0u,
                                   {EAX, STK, STK, STK, STK, STK, STK, STK, STK, STK, STK}};
inline constexpr Function<void(ViewContext *ctx)> FUN_00859ad0{"FUN_00859ad0", 0x00859ad0u, {STK}};
// Unpatched VM stub (0x00854740 -> 0x02537cb0), called before a scratch-record
// Submit; the builder then reads the scratch materialIdx. The registers are
// what the builder's call site holds (VM code: inputs not mapped).
inline constexpr Function<void(void *shell, uint32_t arg, uint32_t *cmd)> thunk_FUN_02537cb0{
    "thunk_FUN_02537cb0", 0x00854740u, {EAX, ECX, EDX}};
// PgPrimitiveScratchRecord_Submit(EAX count, EDX first index, primType, ...,
// materialIdx), cdecl.
inline constexpr Function<void(uint32_t count, uint32_t first, uint32_t p1, uint32_t p2, uint32_t p3, uint32_t p4,
                               uint32_t p5, uint32_t p6)>
    PgPrimitiveScratchRecord_Submit{"PgPrimitiveScratchRecord_Submit",
                                    0x00854910u,
                                    {EAX, EDX, STK, STK, STK, STK, STK, STK}};

// The builder itself: its real entry is the plaintext prologue 0x008591a0
// (push ebp; mov ebp,esp; and esp,-16), which jumps through the call gate
// [0x024cd494] -> 0x00506a26 (sub esp,0xa4) -> the body 0x008591ac; stdcall.
// Reimplemented (reimpl/render/build_camera_constants.hpp); this declares its entry.
inline constexpr Function<void(ViewContext *ctx)> ViewContext_BuildCameraConstants{
    "ViewContext_BuildCameraConstants", 0x008591a0u, {STK}, Pops{4}};

} // namespace mc2vr::game
