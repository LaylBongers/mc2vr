// Per-eye D3D11 render targets + the S4-0 test pattern. Real mode renders
// into the OpenXR swapchain images; mock mode into plain textures.
#pragma once

#include <d3d11.h>

namespace eyes {

// Clears `rtv` with the test pattern for `eye` at frame `n` (left = red-ish,
// right = blue-ish, pulsing so a live HMD shows motion).
void draw_pattern(ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, int eye, unsigned n);

}  // namespace eyes
