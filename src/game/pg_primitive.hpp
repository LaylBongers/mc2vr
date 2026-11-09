// PgPrimitive draw records and the published record list.
//
// Source of truth: the Ghidra struct PgPrimitive (/RenderPath) and the plates
// on g_PrimitiveBase / PgPrimitive_PublishFrame
// (docs/reverse_engineering/render_data_model.md, scene_record_build.md).
// Field names follow Ghidra; rNN = role unknown. Change both together.
#pragma once

#include <cstddef>
#include <cstdint>

#include "game/global.hpp"

namespace mc2vr::game {

struct PgPrimitive {
    void *geometryObj;        // +0x00
    void *streamDataList;     // +0x04
    uint32_t r08;             // +0x08
    void *vertexShader;       // +0x0c
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

} // namespace mc2vr::game
