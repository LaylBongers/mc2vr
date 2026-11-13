// The clouds pass (PgClouds, vtable 0x00bab5b0): the object fields, static
// techniques and passes (Ghidra PgCloudRenderVP / PgCloudRenderFP), globals and
// callees PgClouds_EmitCloudRender1 (0x0047ded0, slot 3) and
// PgClouds_EmitCloudRender2 (0x0047e8c0, slot 6) use. Source of truth: their
// Ghidra plates and types (docs/view_rewrite.md "Clouds").
//
// The PgClouds object is opaque here: offsets from its base. rNN = role unknown.
#pragma once

#include <cstddef>
#include <cstdint>

#include "game/dx9.hpp"
#include "game/function.hpp"
#include "game/global.hpp"
#include "game/pg_primitive.hpp"

namespace mc2vr::game {

// ---- PgClouds fields (floats unless noted) ---------------------------------------------
// The constant block (10 vec4, identical in both emitters) is built from these;
// only the sky matrix depends on the camera.
constexpr uintptr_t kCloudsAspectNum = 0x10;   // c2.xz = +0x10 / +0x14
constexpr uintptr_t kCloudsAspectDen = 0x14;   // also c3.x
constexpr uintptr_t kCloudsCenter = 0x18;      // c3.y/.z = +0x18 +- +0x54 / 2
constexpr uintptr_t kCloudsExtentA = 0x1c;     // c9.xy from (+0x1c, +0x20) * aspect
constexpr uintptr_t kCloudsExtentB = 0x20;
constexpr uintptr_t kCloudsWidth = 0x54;       // c3.w; c3.yz half-width; c9.w factor
constexpr uintptr_t kCloudsShape = 0x5c;       // s: c4.xyz = 3(1-s^2)/(2(s^2+2)), s^2+1, -2s (clamped to [-1, 1])
constexpr uintptr_t kCloudsC4W = 0x60;         // c4.w
constexpr uintptr_t kCloudsScale = 0x64;       // c9.zw = x / 1000, width * x / 1000
constexpr uintptr_t kCloudsTint = 0x6c;        // float[3]: c6.xyz / c7.xyz factors
constexpr uintptr_t kCloudsBlend = 0x78;       // k: c6/c7 = lerp(1, sky colour, k)
constexpr uintptr_t kCloudsRangeLo = 0x7c;     // c8.xy = 1/(hi - lo), lo/(lo - hi)
constexpr uintptr_t kCloudsRangeHi = 0x80;
constexpr uintptr_t kCloudsFadeLo = 0x84;      // c8.zw = hi - lo, lo
constexpr uintptr_t kCloudsFadeHi = 0x88;
constexpr uintptr_t kCloudsMaterialRender1 = 0x260; // the record's material (thunk_FUN_02537cb0, EmitCloudRender1)
constexpr uintptr_t kCloudsMaterialRender2 = 0x3f0; // the record's material (thunk_FUN_02537cb0, EmitCloudRender2)
constexpr uintptr_t kCloudsStreams = 0x690;      // -> the scratch record's streamDataList
constexpr uintptr_t kCloudsGeometry = 0x6a8;     // -> the scratch record's geometryObj
constexpr uintptr_t kCloudsOutRT = 0x6c4;        // IDirect3DSurface9 *: the half-size clouds target (mCloudOutRT)

// The sky parameters (*(g_GlobalCamCtx + 0x104)): +0x00 u8 = 1 skips the
// clouds' own target (they draw into whatever is bound), +0x20 a vec4 copied
// to c5, +0x40 / +0x70 the colours c6 / c7 blend towards.
constexpr uintptr_t kGlobalCamSky = 0x104;
constexpr uintptr_t kSkyC5 = 0x20;
constexpr uintptr_t kSkyColourA = 0x40;
constexpr uintptr_t kSkyColourB = 0x70;

// ---- the shader programs the emitters draw with -------------------------------------
// Static objects, each built by its own static initializer; the instance
// names are the ones Pg_RegisterShaders registers them under
// ("PgCloudRenderVP1" ...). The class names are inferred from them:
//   - CloudRenderVP: a Technique subclass (vtable 0x00bab568) whose resolver
//     (PgCloudRenderVP_ResolveConstantRegisters, slot 4) adds "cQuad0".."cQuad9"
//     and "g_ViewProjMtx"; VP1 and VP2;
//   - CloudRenderFP: an EffectPass subclass (vtable 0x00bab580,
//     PgCloudRenderFP_ResolveConstantRegisters) adding "cQuad0".."cQuad9"; FP1.
// FP2 is a plain EffectPass. Each FP follows its VP in .bss (16-byte
// aligned); they are separate objects.
constexpr uint32_t kCloudsConstCount = 10;
struct CloudRenderVP {                          // Ghidra PgCloudRenderVP
    Technique base;
    ShaderConstParam cQuad[kCloudsConstCount];  // +0x114 c0..c9 <- the constant block
    ShaderConstParam viewProjMtx;               // +0x164 "g_ViewProjMtx" <- the sky matrix
};
static_assert(offsetof(CloudRenderVP, cQuad) == 0x114 && offsetof(CloudRenderVP, viewProjMtx) == 0x164 &&
              sizeof(CloudRenderVP) == 0x16c);
struct CloudRenderFP {                          // Ghidra PgCloudRenderFP
    EffectPass base;
    ShaderConstParam cQuad[kCloudsConstCount];  // +0xfc c0..c9 <- the constant block
};
static_assert(offsetof(CloudRenderFP, cQuad) == 0xfc && sizeof(CloudRenderFP) == 0x14c);

using g_PgCloudRenderVP1 = Global<CloudRenderVP, 0x01284120u>;  // PgClouds_EmitCloudRender1
using g_PgCloudRenderFP1 = Global<CloudRenderFP, 0x01284290u>;  // PgClouds_EmitCloudRender1 (PS cQuad0..9)
using g_PgCloudRenderVP2 = Global<CloudRenderVP, 0x012843e0u>;  // PgClouds_EmitCloudRender2
using g_PgCloudRenderFP2 = Global<EffectPass, 0x01284550u>;     // PgClouds_EmitCloudRender2's pass (no emitter constants)

// ---- globals --------------------------------------------------------------------------
// -1.0 in .data (c9.y = it / (b^2 - a^2)).
using g_CloudsNegOne = Global<float, 0x00dfcb5cu>;
// Cloud render 2's materialIdx: a counter, incremented per emission.
using g_CloudRender2MaterialCounter = Global<uint16_t, 0x01169770u>;

// ---- callees --------------------------------------------------------------------------
// Clamps the object's parameters (+0x50..+0x88) to their ranges. EAX = object.
inline constexpr Function<void(uint8_t *clouds)> PgClouds_ClampParams{"PgClouds_ClampParams", 0x0047c810u, {EAX}};
// RenderShell vtable slot 17 (+0x44), thiscall (shell, technique, &streams, 1):
// a no-op (`ret 0xc`, 0x008306c0) on the live base vtable.
inline constexpr Method<uint32_t(void *shell, const Technique *technique, void *const *streams, uint32_t n)>
    RenderShell_Slot17{"RenderShell vtbl slot 17", 0x44, {ECX, STK, STK, STK}};

// The emitters themselves: thiscall (this, the camera's world matrix: inverted,
// translation zeroed, for the sky matrix), ret 4.
// Reimplemented (reimpl/render/clouds_emit.hpp); these declare their entries.
inline constexpr Function<void(uint8_t *clouds, const float *camera)> PgClouds_EmitCloudRender1{
    "PgClouds_EmitCloudRender1", 0x0047ded0u, {ECX, STK}, Pops{4}};
inline constexpr Function<void(uint8_t *clouds, const float *camera)> PgClouds_EmitCloudRender2{
    "PgClouds_EmitCloudRender2", 0x0047e8c0u, {ECX, STK}, Pops{4}};

} // namespace mc2vr::game
