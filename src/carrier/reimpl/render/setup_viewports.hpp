// RenderShell_SetupViewports (0x0085a9b0) — reimplementation.
//
// STATUS: live; not yet verified against the original.
// Ghidra plate: RenderShell_SetupViewports.
// Original: AX = view mode (1 single, 2 split-screen; the original reads only
// AX), ECX = the render shell; plain `ret`. Callers: LtiRenderer_OnScreenResize,
// PgLtiRendererPc_SetupRenderTargets, 0x0085ab50, 0x005e4dab.
// Entry: game/render_shell.hpp's declaration (hooks/reimpl.cpp).
//
// Writes the view blocks (docs/reverse_engineering/view_blocks.md): block 0 =
// the main viewport {0, 0, W, H}; block 1 = player 1, block 2 = player 2
// (split-screen halves, top/bottom for aspect mode 0, left/right otherwise;
// zeroed in single view). Each block's +0x34 is its UI scale H/480. Then the
// block count +0x2b90 and block 0's post flag +0x3c. Calls nothing.
//
// VR behavior inside it, as a direct call:
//   - vr/hud records the stock UI scales and, while the HUD layer is laid out
//     at 16:9, writes the layer's UI scale and aspect mode (docs/hud.md).
//
// Writes directly: shell +0x20..+0x3c, +0xea0..+0xeb4, +0x1d20..+0x1d34,
// +0x2b90 (and, via vr/hud, the +0x34 scales and +0x2bc4).
#pragma once

#include <cstdint>

namespace mc2vr::reimpl {

void RenderShell_SetupViewports(uint32_t mode, uint8_t *shell);

} // namespace mc2vr::reimpl
