// The post chain's bloom object (PgLtiBloom): the fields, globals and callees
// PgLtiBloom_EmitPasses (0x0074fae0, bloom vtable 0x00bd4a14 slot 1) reads.
// Note: docs/motion_blur.md; Ghidra plate of PgLtiBloom_EmitPasses.
//
// The object is opaque here: offsets from its base. "Material" fields are the
// per-pass material blocks RenderShell_EmitScreenQuad draws with.
#pragma once

#include <cstdint>

#include "game/dx9.hpp"
#include "game/function.hpp"
#include "game/global.hpp"

namespace mc2vr::game {

// ---- PgLtiBloom fields ---------------------------------------------------------------
constexpr uintptr_t kBloomRadius = 0x04;           // float: the blur radius of downsample levels 0 and 1 (x0.4, x0.8)
constexpr uintptr_t kBloomImageParams = 0x08;      // float[2] -> composite imageParams.xy
constexpr uintptr_t kBloomAdaptRate = 0x10;        // float[2] -> adaptation constant .yz (and pow(.y, .z * dt))
constexpr uintptr_t kBloomAdaptParams = 0x18;      // float[4] -> the adaptation pass's second constant
constexpr uintptr_t kBloomFilterParams = 0x28;     // float[3] -> composite filterParams.xyz
constexpr uintptr_t kBloomMotionBlurScale = 0x34;  // float -> composite MotionBlurVelocityScale.x
constexpr uintptr_t kBloomDownsampleMat = 0x460;   // + level * kBloomMatStride (4 levels)
constexpr uintptr_t kBloomBlurAMat = 0xaa0;        // + level * kBloomMatStride: a blur pair's first pass
constexpr uintptr_t kBloomBlurBMat = 0x10e0;       // + level * kBloomMatStride: its second pass
constexpr uintptr_t kBloomLum64Mat = 0x1720;
constexpr uintptr_t kBloomLumMat = 0x18b0;         // + k * kBloomMatStride: the 16x16 and 4x4 steps
constexpr uintptr_t kBloomLum1Mat = 0x1bd0;
constexpr uintptr_t kBloomAdaptMat = 0x1d60;
constexpr uintptr_t kBloomCompositeMat = 0x1ef0;
constexpr uintptr_t kBloomDofMat = 0x2080;         // the half-size pass (inferred: the DOF blur image, blurMap2)
constexpr uintptr_t kBloomMatStride = 400;
// Film grain transform, in the composite material (written per frame).
constexpr uintptr_t kBloomGrainOffset = 0x1f58;    // float[2] random offset
constexpr uintptr_t kBloomGrainScale = 0x1f60;     // float[2]
constexpr uintptr_t kBloomGrainAngle = 0x1f68;     // float, radians
constexpr uintptr_t kBloomGrainRow0 = 0x1f70;      // float[4] (.z untouched)
constexpr uintptr_t kBloomGrainRow1 = 0x1f80;      // float[4] (.z untouched)
// Surfaces (IDirect3DSurface9 *).
constexpr uintptr_t kBloomDownsampleRT = 0x22a0;   // [4]
constexpr uintptr_t kBloomBlurRT = 0x22b0;         // [4] the blur pairs' temporaries
constexpr uintptr_t kBloomAdaptRT = 0x22c4;        // the adapted luminance (this frame)
constexpr uintptr_t kBloomDofRT = 0x22c8;          // the half-size pass's target
constexpr uintptr_t kBloomLum64RT = 0x22d4;
constexpr uintptr_t kBloomLumRT = 0x22d8;          // [2] 16x16, 4x4
constexpr uintptr_t kBloomLum1RT = 0x22e0;
constexpr uintptr_t kBloomAdaptPrevRT = 0x22e4;    // the previous frame's adapted luminance (StretchRect dst)

// ---- globals ---------------------------------------------------------------------------
// The scratch constant buffer the emitters allocate vec4 constants from:
// base pointer and next free byte offset.
using g_ScratchConstBase = Global<uint8_t *, 0x00ff36d0u>;
using g_ScratchConstNext = Global<uint32_t, 0x00ff36c8u>;
// The post-FX grain LCG state (x * 0x19660d + 0x3c6ef35f).
using g_PostFxGrainRng = Global<uint32_t, 0x017d2f5cu>;
// The float 0.95 (a shared constant in .data; the level-3 blur radius here).
using g_Const_0_95 = Global<float, 0x00dfcdc4u>;
// FUN_0074efe0's buffer and count: static locals of PgLtiBloom_EmitPasses,
// zeroed on first use (guard bit 0).
using DAT_019947d0 = GlobalArray<uint32_t, 0x019947d0u, 0x100>;
using DAT_01994bd0 = Global<uint32_t, 0x01994bd0u>;
using DAT_019c5144 = Global<uint32_t, 0x019c5144u>;
// Technique objects the passes draw with: every pass but the composite, and
// the composite (shader3.bin 0x24e990).
using g_PostFxQuadTechnique = Global<uint8_t, 0x0196eef8u>;
using g_PostFxCompositeTechnique = Global<uint8_t, 0x0127b6b0u>;
// PS constant registers of the passes' static effect-pass objects, resolved by
// shader constant name at load (each class's *_ResolveConstants, vtable slot 4):
// the blur pairs' g_PostFxBlurPassA/B ("blurScale"), the luminance steps'
// g_PostFxLumPass / g_PostFxLumFinalPass ("texSize"), g_PostFxAdaptiveLumPass,
// and the composite's g_PostFxCompositePass (0x0127b470, +0xfc..+0x134).
using g_BlurPassA_BlurScale = Global<ShaderConstParam, 0x01971e4cu>;
using g_BlurPassB_BlurScale = Global<ShaderConstParam, 0x0196f82cu>;
using g_LumPass_TexSize = Global<ShaderConstParam, 0x0127b154u>;       // the 16x16 and 4x4 steps
using g_LumFinalPass_TexSize = Global<ShaderConstParam, 0x0127b04cu>;  // the 1x1 step
using g_AdaptiveLum_AdaptiveParams = Global<ShaderConstParam, 0x0127b45cu>;        // dt, rate, pow
using g_AdaptiveLum_MinmaxAdaptiveParams = Global<ShaderConstParam, 0x0127b464u>;  // bloom+0x18..+0x24
using g_CompositeImageParams = Global<ShaderConstParam, 0x0127b56cu>;
using g_CompositeFilterParams = Global<ShaderConstParam, 0x0127b574u>;
using g_CompositeDofParams = Global<ShaderConstParam, 0x0127b57cu>;
using g_CompositeProjectParams = Global<ShaderConstParam, 0x0127b584u>;
using g_CompositeMotionBlurX = Global<ShaderConstParam, 0x0127b58cu>;
using g_CompositeMotionBlurY = Global<ShaderConstParam, 0x0127b594u>;
using g_CompositeMotionBlurW = Global<ShaderConstParam, 0x0127b59cu>;
using g_CompositeMotionBlurScale = Global<ShaderConstParam, 0x0127b5a4u>;

// ---- functions -------------------------------------------------------------------------
// thiscall, no stack arguments, plain ret.
inline constexpr Function<void(uint8_t *bloom)> PgLtiBloom_EmitPasses{"PgLtiBloom_EmitPasses", 0x0074fae0u, {ECX}};
// Submits the scratch record (its cmdStream / viewId) as a w x h screen quad
// drawn with the technique and material; stdcall, ret 0x14.
inline constexpr Function<void(int32_t w, int32_t h, uintptr_t technique, uint8_t *material, uint32_t a5)>
    RenderShell_EmitScreenQuad{"RenderShell_EmitScreenQuad", 0x00853980u, {STK, STK, STK, STK, STK}};
// The sun/lens-flare quads into main (inferred); returns the count drawn. cdecl.
inline constexpr Function<int32_t(uint32_t *quads, uint32_t count)> FUN_0074efe0{"FUN_0074efe0", 0x0074efe0u,
                                                                                 {STK, STK}};

} // namespace mc2vr::game
