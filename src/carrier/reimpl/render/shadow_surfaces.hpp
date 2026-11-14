// The sun shadow atlas's surfaces and its per-cascade pass setup —
// reimplementations, with a configurable atlas resolution (docs/shadows.md).
//
// PgLtiShadow_CreateSurfaces (0x00755f80): thiscall(RenderShell), `ret`;
// called from RenderSystem_CheckDevice (device create/restore). Entry:
// game/render_shell.hpp's declaration (hooks/reimpl.cpp). Creates the depth
// atlas (D24S8 texture when the format flag is set, else DF24, else a plain
// depth surface), the dummy colour RT0 the cascades render with (colour writes
// off) and ShadowMapCombined (never used after creation; kept at the stock
// size), registers them by name, and writes the four cascade RECTs.
//
// RenderCmd_BeginSurfacePass (0x00756420): stdcall(flag, cascade) with the
// gate in EAX, `ret 8`; stream op 16. Its only caller is RenderCmd_ExecuteStream,
// reimplemented, which calls this directly (no entry). Binds the atlas as the
// target, the cascade's viewport (the stock hard-codes 1024x1024), clears the
// cascade's RECT and sets the per-cascade depth bias.
//
// STATUS: new (2026-11-14), not yet live-verified; transcribed from the
// disassembly. Deviations: the engine's debug prints and DXGetErrorString
// become our log; the sizes come from shadow_cascade_size(); a failed texture
// creation is not followed by GetSurfaceLevel on NULL (the original crashes).
//
// conf shadow_resolution = 1024 (stock, default) | 2048 | 4096: the edge of
// each of the four cascades. The 9-tap receivers' texel constants follow
// (hooks/shader_reimpl.cpp), and so does the stabilized fit
// (fit_shadow_cascades.cpp). Larger costs VRAM (depth 64 / 256 MB) and
// shadow-pass fill rate.
#pragma once

#include <cstdint>

namespace mc2vr::reimpl {

bool set_shadow_resolution(const char *value);
uint32_t shadow_cascade_size();  // the edge of one cascade (1024 stock)

void PgLtiShadow_CreateSurfaces(uint8_t *shell);
void RenderCmd_BeginSurfacePass(uint32_t flag, uint32_t cascade, uint32_t gate);

} // namespace mc2vr::reimpl
