// PgPrimitive draw records and the published record list.
//
// Source of truth: the Ghidra struct PgPrimitive (/RenderPath) and the plates
// on g_PrimitiveBase / PgPrimitive_PublishFrame
// (docs/reverse_engineering/render_data_model.md, scene_record_build.md).
// Field names follow Ghidra; rNN = role unknown. Change both together.
#pragma once

#include <cstddef>
#include <cstdint>

#include "game/dx9.hpp"
#include "game/global.hpp"

namespace mc2vr::game {

struct PgGeometry;

struct PgPrimitive {
    PgGeometry *geometryObj;  // +0x00
    void *streamDataList;     // +0x04
    uint32_t r08;             // +0x08
    IDirect3DVertexShader9 *vertexShader; // +0x0c
    uint32_t primType;        // +0x10
    uint32_t minIndex;        // +0x14
    uint32_t r18;             // +0x18
    uint32_t startVertexIndex;// +0x1c
    uint32_t primCount;       // +0x20
    uint32_t numVertices;     // +0x24
    uint32_t streamSetId;     // +0x28
    uint32_t *cmdStream;      // +0x2c  RenderCmd dword stream (RenderCmd_ExecuteStream)
    uint8_t stencilMode;      // +0x30
    uint8_t r31[3];           // +0x31
    uint32_t r34;             // +0x34
    uint16_t envIdx;          // +0x38
    uint16_t viewIdx;         // +0x3a
    uint16_t materialIdx;     // +0x3c
    uint16_t r3e;             // +0x3e
    uint16_t techniqueIdx;    // +0x40
    uint16_t streamCount;     // +0x42
    uint8_t viewType;         // +0x44
    uint8_t r45;              // +0x45
    uint16_t viewId;          // +0x46
    uint8_t rtWriteMask;      // +0x48
    uint8_t viewContextIdx;   // +0x49
    uint16_t screenIdx;       // +0x4a
    uint16_t objIdx;          // +0x4c
    uint16_t r4e;             // +0x4e
    float *vsConstData;       // +0x50
    uint32_t vsConstCount;    // +0x54
};

static_assert(sizeof(void *) == 4, "game structs assume the i386 build");
static_assert(sizeof(PgPrimitive) == 0x58);
static_assert(offsetof(PgPrimitive, cmdStream) == 0x2c);
static_assert(offsetof(PgPrimitive, stencilMode) == 0x30);
static_assert(offsetof(PgPrimitive, envIdx) == 0x38);
static_assert(offsetof(PgPrimitive, techniqueIdx) == 0x40);
static_assert(offsetof(PgPrimitive, viewType) == 0x44);
static_assert(offsetof(PgPrimitive, rtWriteMask) == 0x48);
static_assert(offsetof(PgPrimitive, viewContextIdx) == 0x49);
static_assert(offsetof(PgPrimitive, screenIdx) == 0x4a);
static_assert(offsetof(PgPrimitive, vsConstData) == 0x50);
static_assert(offsetof(PgPrimitive, vsConstCount) == 0x54);

// Sort-key/next table entry (g_PrimitiveSortKeys): 6-byte sort key, then the
// next record index of the sorted list (0xffff terminates).
struct PrimSortEntry {
    uint16_t key[3];
    uint16_t next;
};
static_assert(sizeof(PrimSortEntry) == 8);

constexpr uint32_t kPrimitivesPerBuffer = 0x2000;  // per double-buffer half
constexpr uint16_t kPrimListEnd = 0xffff;

// Published (read-side) list, valid from PgPrimitive_PublishFrame to the next one.
using g_PrimitiveBase = Global<PgPrimitive *, 0x0116977cu>;
using g_PrimitiveHead = Global<uint16_t, 0x01169780u>;
using g_PrimitiveSortKeys = GlobalArray<PrimSortEntry, 0x01153700u, kPrimitivesPerBuffer>;
// Write side: double-buffer index (ping-pong 0/1) and records written this frame.
using g_PrimBufferIndex = Global<uint32_t, 0x00ff364cu>;
using g_PrimWriteCount = Global<uint32_t, 0x00ff36f0u>;

// ---- what a record refers to ---------------------------------------------------
// Partial layouts: only what PgPrimitive_SubmitToGPU reads. rNNN = unknown.
// Shader-constant parameter names say what the submit uploads into them.

// Technique (vertex side), g_TechniqueTable[record.techniqueIdx].
struct Technique {
    uint8_t r000[0x94];
    ShaderConstParam envConsts;          // +0x94  <- the record's env entry
    uint8_t r09c[0xac - 0x9c];
    ShaderConstParam recordConsts;       // +0xac  <- record.vsConstData (count*3 vec4s, direct device call)
    ShaderConstParam screenConsts;       // +0xb4  <- g_ScreenConstsVS
    ShaderConstParam materialConsts;     // +0xbc  <- material + 0x80
    uint8_t r0c4[0xcc - 0xc4];
    ShaderConstParam globalLight;        // +0xcc  <- viewContext.globalLightData
    ShaderConstParam viewContextData;    // +0xd4  <- the viewContext record (the camera block)
    uint8_t r0dc[0xe4 - 0xdc];
    ShaderConstParam atmosphere;         // +0xe4  <- viewContext.atmosphereData
    ShaderConstParam atmosphereVec;      // +0xec  <- atmosphereData + 0x60 (one vec4, direct device call)
    ShaderConstParam materialExtra;      // +0xf4  <- material.vsConst170
};
static_assert(offsetof(Technique, envConsts) == 0x94 && offsetof(Technique, recordConsts) == 0xac &&
              offsetof(Technique, viewContextData) == 0xd4 && offsetof(Technique, materialExtra) == 0xf4);

// Effect pass (pixel side): g_EffectPassTable entry or a RenderShell_PickPass_* result.
struct EffectPass {
    uint8_t r000[0x94];
    ShaderConstParam screenBoolsA;       // +0x94 (B) <- screenView + 0x280
    ShaderConstParam screenBoolsB;       // +0x9c (B) <- screenView + 0x290
    ShaderConstParam screenConstsA;      // +0xa4  <- screenView + 0x000
    ShaderConstParam screenConstsB;      // +0xac  <- screenView + 0x180
    ShaderConstParam globalLight;        // +0xb4  <- viewContext.globalLightData
    ShaderConstParam materialMatrix;     // +0xbc  <- 6 vec4: material rows x view scale/offset (or the material)
    ShaderConstParam cameraPos;          // +0xc4  <- viewContext + 0x40
    ShaderConstParam materialExtra;      // +0xcc  <- material.psConst178
    ShaderConstParam screenConsts;       // +0xd4  <- g_ScreenConstsPS
    ShaderConstParam viewContextData;    // +0xdc  <- the viewContext record
    uint8_t r0e4[0xec - 0xe4];
    ShaderConstParam stencilFlag;        // +0xec  <- {1 or 0, 0, 0, 0} (direct device call)
};
static_assert(offsetof(EffectPass, screenBoolsA) == 0x94 && offsetof(EffectPass, materialMatrix) == 0xbc &&
              offsetof(EffectPass, viewContextData) == 0xdc && offsetof(EffectPass, stencilFlag) == 0xec);

struct PgMaterial {
    float rows[6][4];                    // +0x00  scaled by the view entry into EffectPass.materialMatrix
    uint8_t r060[0x80 - 0x60];
    uint8_t vsConsts[0x20];              // +0x80  -> Technique.materialConsts
    uint8_t flagsA0;                     // +0xa0  0x02: scale rows 0-4 by row5.w; 0x0e: zero atmosphereVec.y
    uint8_t r0a1[0x170 - 0xa1];
    const void *vsConst170;              // +0x170
    uint8_t r174[4];
    const void *psConst178;              // +0x178
    uint8_t r17c[0x182 - 0x17c];
    uint16_t effectPass;                 // +0x182 base index into g_EffectPassTable
};
static_assert(offsetof(PgMaterial, vsConsts) == 0x80 && offsetof(PgMaterial, flagsA0) == 0xa0 &&
              offsetof(PgMaterial, vsConst170) == 0x170 && offsetof(PgMaterial, psConst178) == 0x178 &&
              offsetof(PgMaterial, effectPass) == 0x182);

// g_ViewContextTable entries (0x70; layout: docs/reverse_engineering/view_context_records.md).
struct ViewContextRecord {
    uint8_t r00[0x40];
    float cameraPos[4];                  // +0x40
    uint8_t r50[0x60 - 0x50];
    const uint8_t *atmosphereData;       // +0x60 (its +0x60 vec4 -> Technique.atmosphereVec)
    const void *globalLightData;         // +0x64
    uint8_t r68[0x70 - 0x68];
};
static_assert(sizeof(ViewContextRecord) == 0x70 && offsetof(ViewContextRecord, atmosphereData) == 0x60);

struct ScreenView {                      // g_ScreenViewTable entries
    uint8_t constsA[0x180];              // +0x000
    uint8_t constsB[0x100];              // +0x180
    uint8_t boolsA[0x10];                // +0x280
    uint8_t boolsB[0x10];                // +0x290
    uint32_t effectPassOffset;           // +0x2a0 added to PgMaterial.effectPass (0 when screen effects are off)
    uint8_t r2a4[0x2b0 - 0x2a4];
};
static_assert(sizeof(ScreenView) == 0x2b0 && offsetof(ScreenView, effectPassOffset) == 0x2a0);

struct ViewEntry {                       // g_ViewTable entries
    float scale[4];
    float offset[4];
};
static_assert(sizeof(ViewEntry) == 0x20);

struct PgGeometry {                      // PgPrimitive.geometryObj
    void *r00;
    IDirect3DIndexBuffer9 *indices;      // +0x04
};

using g_ScreenViewTable = Global<ScreenView *, 0x01163760u>;
using g_ViewTable = Global<ViewEntry *, 0x01169778u>;
using g_ViewContextTable = Global<ViewContextRecord *, 0x01169774u>;
using g_EnvTable40 = Global<uint8_t *, 0x00ff36f8u>;          // 0x40-byte entries
using g_MaterialTable = Global<PgMaterial **, 0x00ff36f4u>;
using g_EffectPassTable = GlobalArray<EffectPass *, 0x0197aa40u, 0x10000>;  // indexed by int16
using g_TechniqueTable = GlobalArray<Technique *, 0x0197d248u, 0x100>;
using g_ScreenConstsVS = Global<const uint8_t, 0x017d2aa0u>;  // address only
using g_ScreenConstsPS = Global<const uint8_t, 0x017d2ab0u>;  // address only
using g_ScreenEffectsEnabled = Global<uint8_t, 0x00dfb345u>;
using g_OcclusionQueryArray = Global<IDirect3DQuery9 **, 0x01163768u>;

// ---- draw-state cache (block at 0x01169788, reset by RenderShell_ResetDrawStateCache) ----
// Each submit compares a record against these to skip redundant state.
using g_LastEnvIdx = Global<uint16_t, 0x01169788u>;
using g_LastScreenIdx = Global<uint16_t, 0x0116978au>;
using g_LastMaterialIdx = Global<uint16_t, 0x0116978cu>;
using g_LastViewContextIdx = Global<uint16_t, 0x0116978eu>;
using g_LastViewIdx = Global<uint16_t, 0x01169790u>;
// 0x01169792 g_CurViewType (u8): game/render_shell.hpp
using g_LastViewId = Global<uint16_t, 0x01169794u>;
using g_LastTechnique = Global<Technique *, 0x01169798u>;
using g_LastPass = Global<EffectPass *, 0x0116979cu>;
using g_LastGeometry = Global<PgGeometry *, 0x011697b0u>;
using g_LastVertexShader = Global<IDirect3DVertexShader9 *, 0x011697b4u>;
using g_CurRTWriteMask = Global<uint8_t, 0x011697bcu>;   // 4 = "unknown"
using g_CurStencilMode = Global<uint8_t, 0x011697bdu>;
constexpr uintptr_t kDrawStateCacheBlock = 0x01169788u;

} // namespace mc2vr::game
