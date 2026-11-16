// RenderShell_ApplyViewSamplerStates (0x00751240) — reimplementation, with
// anisotropic filtering for asset textures (docs/texture_filtering.md).
//
// STATUS: live; transcribed from the disassembly. Verified outside the HMD
// (2026-11-16): the per-class decisions match the rule and the image is
// visibly better. Not yet run in the HMD. Ghidra plate: RenderShell_ApplyViewSamplerStates.
// Original: ESI = sampler (pixel 0-15, vertex 257-260), TextureView * on the
// stack, `ret 4`. Called right after Dx9_SetTexture(view->texture) by
// RenderShell_BindViewTexture and RenderShell_PickPass_Material (both
// original, so both reach ours through the entry).
// Entry: game/render_shell.hpp's declaration (hooks/reimpl.cpp).
//
// Sets the 7 filter/address/LOD-bias sampler states from view->samplerFlags
// (game/texture.hpp), all through Dx9_SetSamplerState (the engine's sampler
// cache). Calls nothing else.
//
// Feature behavior inside it: a linear-filtered 2D asset texture with more
// than one resident mip level, on a pixel sampler, gets MINFILTER ANISOTROPIC
// and MAXANISOTROPY = the configured level (clamped to the device caps). Every
// other bind (render targets, CPU-made textures, cubes, vertex samplers,
// point-filtered or single-level textures) gets exactly the stock states.
//
// conf anisotropic_filtering = off (stock) | 2 | 4 | 8 | 16 (default).
//
// Writes directly: nothing (the cache is written by Dx9_SetSamplerState).
#pragma once

#include <cstdint>

#include "game/texture.hpp"

namespace mc2vr::reimpl {

bool set_anisotropic_filtering(const char *value);

void RenderShell_ApplyViewSamplerStates(uint32_t sampler, const game::TextureView *view);

} // namespace mc2vr::reimpl
