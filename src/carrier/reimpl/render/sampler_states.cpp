// RenderShell_ApplyViewSamplerStates — see sampler_states.hpp.
//
// Follows the original call for call (VA ranges in the comments); with
// anisotropic_filtering=off the device sees the stock sequence.

#include "reimpl/render/sampler_states.hpp"

#include <d3d9.h>

#include <cstdint>
#include <cstring>

#include "game/dx9.hpp"
#include "log.hpp"

namespace mc2vr::reimpl {

using namespace game;

namespace {

uint32_t g_requested = 16;  // conf; 0 = off
uint32_t g_level = 0;       // resolved against the device caps; 0 = off
bool g_resolved = false;

// The level to use, resolved once against the device that is now bound.
// Render thread only (like every caller of this file).
uint32_t max_anisotropy()
{
    if (g_resolved) {
        return g_level;
    }
    g_resolved = true;
    if (g_requested == 0) {
        return g_level = 0;
    }
    IDirect3DDevice9 *const dev = lti_device();
    D3DCAPS9 caps{};
    if (dev == nullptr || FAILED(dev->GetDeviceCaps(&caps))) {
        g_resolved = dev != nullptr;  // no device yet: ask again on the next bind
        MC2VR_LOG("aniso: device caps unavailable — anisotropic filtering off");
        return 0;
    }
    if ((caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFANISOTROPIC) == 0 || caps.MaxAnisotropy < 2) {
        MC2VR_LOG("aniso: device reports no anisotropic minification (MaxAnisotropy %lu) — off",
                  (unsigned long)caps.MaxAnisotropy);
        return g_level = 0;
    }
    g_level = g_requested < caps.MaxAnisotropy ? g_requested : (uint32_t)caps.MaxAnisotropy;
    MC2VR_LOG("aniso: anisotropic filtering %ux (requested %u, device max %lu)", g_level, g_requested,
              (unsigned long)caps.MaxAnisotropy);
    return g_level;
}

// Linear-filtered textures that get anisotropic minification: mipmapped 2D
// assets on pixel samplers (docs/texture_filtering.md).
bool wants_anisotropy(uint32_t sampler, const TextureView *view)
{
    return sampler < 16 && view->type == kTexType2DAsset && view->residentLevels > 1;
}

// Logs the first bind of each texture class with its decision (at most 64
// lines a session): which kinds of textures get anisotropic filtering.
void log_class_once(uint32_t sampler, const TextureView *view, uint32_t level)
{
    static uint64_t seen = 0;
    const uint32_t key = (sampler >= 16 ? 1u : 0u) | ((view->type & 7u) << 1) |
                         (view->residentLevels > 1 ? 16u : 0u) | ((view->samplerFlags & kTexFlagPoint) ? 32u : 0u);
    if (seen & (1ull << key)) {
        return;
    }
    seen |= 1ull << key;
    // The record's own fields can be stale (type-4 records never write
    // levels/format), so also report the device texture itself.
    unsigned d3d_type = 0, d3d_levels = 0, usage = 0, fmt = 0, w = 0, h = 0;
    if (IDirect3DBaseTexture9 *const tex = view->texture) {
        d3d_type = (unsigned)tex->GetType();
        d3d_levels = (unsigned)tex->GetLevelCount();
        D3DSURFACE_DESC desc{};
        if (d3d_type == D3DRTYPE_TEXTURE &&
            SUCCEEDED(static_cast<IDirect3DTexture9 *>(tex)->GetLevelDesc(0, &desc))) {
            usage = (unsigned)desc.Usage, fmt = (unsigned)desc.Format, w = desc.Width, h = desc.Height;
        }
    }
    MC2VR_LOG("aniso: class %s type=%u mips=%s point=%u -> %s (first: sampler %u, %ux%u, %u/%u levels, "
              "format %08x, flags %04x; device: rtype %u, %u levels, %ux%u, format %08x, usage %08x)",
              sampler >= 16 ? "vertex" : "pixel", (unsigned)view->type, view->residentLevels > 1 ? "multi" : "single",
              (unsigned)(view->samplerFlags & kTexFlagPoint), level != 0 ? "ANISOTROPIC" : "stock", sampler,
              (unsigned)view->width, (unsigned)view->height, (unsigned)view->residentLevels, (unsigned)view->levels,
              (unsigned)view->format, (unsigned)view->samplerFlags, d3d_type, d3d_levels, w, h, fmt, usage);
}

} // namespace

bool set_anisotropic_filtering(const char *value)
{
    if (std::strcmp(value, "off") == 0) g_requested = 0;
    else if (std::strcmp(value, "2") == 0) g_requested = 2;
    else if (std::strcmp(value, "4") == 0) g_requested = 4;
    else if (std::strcmp(value, "8") == 0) g_requested = 8;
    else if (std::strcmp(value, "16") == 0) g_requested = 16;
    else return false;
    g_resolved = false;
    MC2VR_LOG("aniso: anisotropic_filtering=%s", value);
    return true;
}

void RenderShell_ApplyViewSamplerStates(uint32_t sampler, const TextureView *view)
{
    const uint16_t flags = view->samplerFlags;
    // Feature: anisotropic minification for mipmapped 2D assets.
    const uint32_t level = (flags & kTexFlagPoint) == 0 && wants_anisotropy(sampler, view) ? max_anisotropy() : 0;
    log_class_once(sampler, view, level);

    // ---- filters 0x751245-0x75128f ----
    uint32_t mip;
    if (flags & kTexFlagPoint) {
        Dx9_SetSamplerState(sampler, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        Dx9_SetSamplerState(sampler, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        mip = D3DTEXF_NONE;
    } else {
        Dx9_SetSamplerState(sampler, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        if (level != 0) {
            Dx9_SetSamplerState(sampler, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC);
            Dx9_SetSamplerState(sampler, D3DSAMP_MAXANISOTROPY, level);
        } else {
            Dx9_SetSamplerState(sampler, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        }
        mip = (flags & kTexFlagMipPoint) ? D3DTEXF_POINT : D3DTEXF_LINEAR;
    }
    Dx9_SetSamplerState(sampler, D3DSAMP_MIPFILTER, mip);

    // ---- addressing 0x751294-0x7512e3 ----
    Dx9_SetSamplerState(sampler, D3DSAMP_ADDRESSU, (flags & kTexFlagClampU) ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP);
    Dx9_SetSamplerState(sampler, D3DSAMP_ADDRESSV, (flags & kTexFlagClampV) ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP);
    Dx9_SetSamplerState(sampler, D3DSAMP_ADDRESSW, (flags & kTexFlagClampW) ? D3DTADDRESS_CLAMP : D3DTADDRESS_WRAP);

    // ---- LOD bias 0x7512e8-0x7512f2 (the float's bits, as the original) ----
    Dx9_SetSamplerState(sampler, D3DSAMP_MIPMAPLODBIAS, view->mipLodBias);
}

} // namespace mc2vr::reimpl
