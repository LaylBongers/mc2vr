// The engine's texture record, as the sampler binds read it. Layout: Ghidra
// /RenderPath/TextureView; producers and the RE trail on its comment and in
// docs/reverse_engineering/texture_filtering.md.
#pragma once

#include <d3d9.h>

#include <cstddef>
#include <cstdint>

namespace mc2vr::game {

// TextureView.samplerFlags bits (read by RenderShell_ApplyViewSamplerStates).
// 0x10 / 0x20 / 0x80 are set by some asset files; no reader found.
enum : uint16_t {
    kTexFlagPoint = 0x01,     // MIN/MAG POINT, MIP NONE
    kTexFlagClampU = 0x02,    // else WRAP
    kTexFlagClampV = 0x04,
    kTexFlagClampW = 0x08,
    kTexFlagMipPoint = 0x40,  // MIP POINT instead of LINEAR
};

// TextureView.type.
enum : uint8_t {
    kTexTypeCpu = 0,           // created at runtime, single level (constructor default)
    kTexType2DAsset = 1,       // loaded from a texture file
    kTexTypeCubeAsset = 2,
    kTexTypeRenderTarget = 4,  // render targets (D3DUSAGE_RENDERTARGET, live-confirmed);
                               // levels/residentLevels/format are stale on these
};

// 0x34 bytes; embedded at +0x1c of g_TextureRegistry entries, standalone for
// render-target records (bloom, ColorBufferCopy, ...).
struct TextureView {
    void *vftable;                   // +0x00 stub vtable 0x00bd4c60
    uint16_t width;                  // +0x04 0xffff for render targets
    uint16_t height;                 // +0x06
    uint16_t depth;                  // +0x08
    uint16_t levels;                 // +0x0a mip levels in the file / as created
    uint16_t residentLevels;         // +0x0c levels actually loaded (streaming quality)
    uint16_t pad_0e;
    IDirect3DBaseTexture9 *texture;  // +0x10
    IDirect3DBaseTexture9 *prevTexture;  // +0x14 kept during stream-in
    uint32_t byteSize;               // +0x18
    uint32_t prevByteSize;           // +0x1c
    uint32_t mipLodBias;             // +0x20 float bits, passed to the device as is
    D3DFORMAT format;                // +0x24
    uint16_t samplerFlags;           // +0x28 kTexFlag*
    uint8_t type;                    // +0x2a kTexType*
    uint8_t pad_2b;
    uint16_t qualityMipMask[4];      // +0x2c per streaming quality level
};
static_assert(offsetof(TextureView, levels) == 0x0a);
static_assert(offsetof(TextureView, residentLevels) == 0x0c);
static_assert(offsetof(TextureView, texture) == 0x10);
static_assert(offsetof(TextureView, mipLodBias) == 0x20);
static_assert(offsetof(TextureView, samplerFlags) == 0x28);
static_assert(offsetof(TextureView, type) == 0x2a);
static_assert(sizeof(TextureView) == 0x34);

} // namespace mc2vr::game
