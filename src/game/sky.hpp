// The sky emitter Sky_EmitRecords (0x0047f890, a vtable slot at 0x00bab634):
// the sky object's fields, its shader objects, globals and callees. Source of
// truth: its Ghidra plate (docs/reverse_engineering/sky_clouds.md).
//
// The sky object is opaque here: offsets from its base.
#pragma once

#include <cstddef>
#include <cstdint>

#include "game/dx9.hpp"
#include "game/function.hpp"
#include "game/global.hpp"
#include "game/pg_primitive.hpp"

namespace mc2vr::game {

// ---- sky object fields ------------------------------------------------------------------
constexpr uintptr_t kSkyMaterialDomeHaze = 0x010;  // the dome's material with the haze (sky flag clear)
constexpr uintptr_t kSkyMaterialDome = 0x1a0;      // the dome's material without it (sky flag set)
constexpr uintptr_t kSkyMaterialSun = 0x330;
constexpr uintptr_t kSkyMaterialWindow = 0x4c0;    // the depth-window record
constexpr uintptr_t kSkyMaterialRestore = 0x650;   // the viewport-restore record
constexpr uintptr_t kSkyClouds = 0x7e0;            // -> the clouds object (vtable 0x00bab5b0)
constexpr uintptr_t kSkyStreams = 0x7f0;           // void *[3]: the dome mesh's stream data, per LOD
constexpr uintptr_t kSkyGeometry = 0x7fc;          // PgGeometry *[3]: the dome mesh, per LOD

// ---- constants ---------------------------------------------------------------------------
constexpr float kSkyFarFraction = 0.8f;      // [0x00b9852c] the window's depth and the haze start, x far
constexpr float kSkyHazeEndFraction = 0.99f; // [0x00d2c838] the haze end, x far
constexpr float kSkyDomeScaleXZ = 20000.0f;  // [0x00b9c5d0] PgSkyVP "scale" .x/.z
constexpr float kSkyDomeScaleY = 10000.0f;   // [0x00bea6e8] PgSkyVP "scale" .y
constexpr float kSunOffset = 16.0f;          // [0x00b9ba6c] the sun quad's distance along the sun direction
constexpr float kSunDistance = 15000.0f;     // [0x00ba87ec] the sun constant's distance (x kSunOffset)
constexpr float kSunQuadScale = 0.8f;        // [0x00b9852c] the sun quad's scale

// ---- the shader programs ------------------------------------------------------------------
// Static objects registered by Pg_RegisterShaders under these names; the
// subclasses append constant parameters.
struct SkyVP {                                // "PgSkyVP", vtable 0x00bab5e8
    Technique base;
    ShaderConstParam scale;                   // +0x114 "scale" (c18) <- (20000, 10000, 20000, 1)
};
static_assert(offsetof(SkyVP, scale) == 0x114 && sizeof(SkyVP) == 0x11c);
// Two parameters: +0x114 (unused by the emitter; presumably inherited from
// SkyVP, inferred from the layout only) and its own. Its shipped shader's
// constant table has only viewContextData and objectData, so neither has a
// register at runtime (count 0: the upload is skipped).
struct SunVP {                                // "PgSunVP", vtable 0x00bab618
    Technique base;
    ShaderConstParam r114;                    // +0x114
    ShaderConstParam sunPosition;             // +0x11c <- the sun at kSunDistance
};
static_assert(offsetof(SunVP, r114) == 0x114 && offsetof(SunVP, sunPosition) == 0x11c);
static_assert(offsetof(SunVP, sunPosition) + 8 <= 0x12849b8u - 0x01284890u);  // ends before PgSkyFP
struct SkyFP {                                // "PgSkyFP"
    EffectPass base;
    ShaderConstParam projectParams;           // +0xfc "projectParams" (c1) <- (A, B, haze start, 1 / haze length)
};
static_assert(offsetof(SkyFP, projectParams) == 0xfc);

using g_PgSkyVP = Global<SkyVP, 0x01284770u>;
using g_PgSunVP = Global<SunVP, 0x01284890u>;
using g_PgSkyFP = Global<SkyFP, 0x012849b8u>;

// ---- globals --------------------------------------------------------------------------
// The dome's LOD index (2 in every run) and the segment count per LOD
// (16 at LOD 2); the dome mesh has (n + 1)^2 vertices.
using g_SkyDomeLod = Global<uint32_t, 0x00d29efcu>;
using g_SkyDomeSegments = GlobalArray<int32_t, 0x00d29e74u, 3>;
// RenderShell +0x2b94: -> u8, nonzero = the view is the water reflection; its
// viewport is then (0, 0, water +0x14, water +0x18).
constexpr uintptr_t kRenderShellReflectionFlag = 0x2b94;
using g_PgWater = Global<uint8_t *, 0x00d69f80u>;
constexpr uintptr_t kWaterReflectionW = 0x14;
constexpr uintptr_t kWaterReflectionH = 0x18;
// While set, PgPrimitiveScratchRecord_Submit puts every record into the
// scratch record's rtWriteMask byte (+0x48) as its sort bucket, instead of
// the one its type selects.
using g_ScratchBucketOverride = Global<uint8_t, 0x011755c1u>;

// ---- callees --------------------------------------------------------------------------
// Rows 0..2 of `out` = an orthonormal frame whose row 2 is normalize(dir.xyz);
// row 3 is kept. EAX = dir, ret 4.
inline constexpr Function<void(const float *dir, float *out)> Matrix_OrientFromDirection{
    "Matrix_OrientFromDirection", 0x008255f0u, {EAX, STK}};
inline constexpr Function<float *(float *out, float sx, float sy, float sz)> D3DXMatrixScaling{
    "D3DXMatrixScaling", 0x0096bda6u, {STK, STK, STK, STK}};
// The clouds object's vtable slot 2 (+0x08): tail-jumps to slot 6,
// PgClouds_EmitCloudRender2. thiscall (clouds, camera world matrix).
inline constexpr Method<void(uint8_t *clouds, const float *camera)> PgClouds_Slot2{"PgClouds vtbl slot 2", 0x08,
                                                                                   {ECX, STK}};
constexpr uintptr_t kPgCloudsVtable = 0x00bab5b0u;
constexpr uintptr_t kPgCloudsSlot6 = 0x00bab5c8u;  // -> PgClouds_EmitCloudRender2

// The emitter itself: thiscall (sky, the camera's world matrix), ret 4.
// Reimplemented (reimpl/render/sky_emit.hpp); this declares its entry.
inline constexpr Function<void(uint8_t *sky, const float *camera)> Sky_EmitRecords{
    "Sky_EmitRecords", 0x0047f890u, {ECX, STK}, Pops{4}};

} // namespace mc2vr::game
