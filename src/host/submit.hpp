// S4-3: copy the carrier's shared-eye images into the OpenXR swapchains
// (docs/plans/stereo_design.md §S4). One fullscreen
// triangle with a tiny sample-and-write shader: the source SRV and the
// swapchain RTV are both PLAIN-UNORM-cast views, so the sRGB-encoded LDR
// finals pass through byte-exact — the runtime's compositor then decodes
// the sRGB-typed swapchain, which is exactly what our display-referred
// content means. (CopyResource is illegal across UNORM<->sRGB, and the
// runtime offers sRGB-only 8-bit formats — S4-0, live-verified.)
//
// The draw letterboxes: the shared pair is 2560x1440 (16:9) while the eye
// images are e.g. 2016x2240, so the image is scaled to fit and centered on
// black. All entry points are safe no-ops when init() failed — the frame
// loop then falls back to the S4-0 test pattern, which doubles as the
// "carrier not talking" diagnostic.
#pragma once

#include <d3d11.h>
#include <cstdint>

namespace sub {

// Compile the shaders (d3dcompiler_47 loaded dynamically — the Proton prefix
// ships it; no link-time import). Logs and returns false on any failure.
bool init(ID3D11Device* dev);

bool ready();

// Blit `srcW x srcH` (shared texture, via its UNOM-cast SRV) into the
// swapchain image `rtv` (`dstW x dstH`). stretch=false: aspect-fit with black
// borders (static-pan frames). stretch=true: fill the whole image — used for
// HMD-pose frames, whose projection the carrier rebuilt from the eye FOV, so
// the non-uniform stretch is the exact inverse of the squeeze at render time.
// No-op unless init() succeeded.
void draw(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* srv,
          ID3D11RenderTargetView* rtv, uint32_t srcW, uint32_t srcH,
          uint32_t dstW, uint32_t dstH, bool stretch);

}  // namespace sub
