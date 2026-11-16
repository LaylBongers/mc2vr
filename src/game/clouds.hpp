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
// only the sky matrix depends on the camera. Roles from the shaders
// (PgCloudRenderVP1 / FP1, docs/reverse_engineering/sky_clouds.md; live
// values in parentheses). The cloud layer is the clouds' mesh scaled by
// kCloudsPlaneExtent around the camera, at kCloudsAltitude above it,
// kCloudsThickness thick.
constexpr uintptr_t kCloudsTexRepeat = 0x10;    // texture repeats per extent (1.25): c2.xz = it / extent
constexpr uintptr_t kCloudsPlaneExtent = 0x14;  // the mesh's scale (7000): c3.x
constexpr uintptr_t kCloudsAltitude = 0x18;     // the layer's middle above the camera (750): c3.yz = it +- thickness / 2
constexpr uintptr_t kCloudsFadeStart = 0x1c;    // radial fade, full opacity inside (world units; x c2 into texture units)
constexpr uintptr_t kCloudsFadeEnd = 0x20;      // radial fade, zero outside: c9.xy
constexpr uintptr_t kCloudsThickness = 0x54;    // (125): c3.w; c9.w factor
constexpr uintptr_t kCloudsPhaseG = 0x5c;       // Cornette-Shanks anisotropy g: c4.xyz = 3(1-g^2)/(2(g^2+2)), g^2+1, -2g
constexpr uintptr_t kCloudsScatter = 0x60;      // the phase function's strength: c4.w
constexpr uintptr_t kCloudsExtinction = 0x64;   // per 1000 units of density: c9.zw = it / 1000, thickness * it / 1000
constexpr uintptr_t kCloudsTint = 0x6c;         // float[3]: c6.xyz / c7.xyz factors
constexpr uintptr_t kCloudsBlend = 0x78;        // k: c6/c7 = lerp(1, sky colour, k)
constexpr uintptr_t kCloudsDensityLo = 0x7c;    // density [lo, hi] -> 0..1: c8.xy = 1/(hi - lo), lo/(lo - hi)
constexpr uintptr_t kCloudsDensityHi = 0x80;
constexpr uintptr_t kCloudsOpacityLo = 0x84;    // ... -> opacity [lo, hi]: c8.zw = hi - lo, lo
constexpr uintptr_t kCloudsOpacityHi = 0x88;
constexpr uintptr_t kCloudsMaterialRender1 = 0x260; // the record's material (thunk_FUN_02537cb0, EmitCloudRender1)
constexpr uintptr_t kCloudsMaterialRender2 = 0x3f0; // the record's material (thunk_FUN_02537cb0, EmitCloudRender2)
constexpr uintptr_t kCloudsStreams = 0x690;      // -> the scratch record's streamDataList
constexpr uintptr_t kCloudsGeometry = 0x6a8;     // -> the scratch record's geometryObj
constexpr uintptr_t kCloudsOutRT = 0x6c4;        // IDirect3DSurface9 *: the half-size clouds target (mCloudOutRT)

// The sky parameters (*(g_GlobalCamCtx + 0x104)): +0x00 u8 = 1 skips the
// clouds' own target, the haze and cloud render 2; +0x20 the sun direction
// (the clouds' c5, the sun quad's placement); +0x40 / +0x70 the colours c6 /
// c7 blend towards (c6 lights the clouds in FP1; c7 is read by no shipped
// cloud shader).
constexpr uintptr_t kGlobalCamSky = 0x104;
constexpr uintptr_t kSkySunDirection = 0x20;
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
