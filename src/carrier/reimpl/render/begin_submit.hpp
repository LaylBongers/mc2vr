// LtiRenderer_BeginSubmit (0x0074aaa0) — reimplementation.
//
// STATUS: live; verified in gameplay, not compared call for call (docs/reimpl.md).
// Ghidra plate: LtiRenderer_BeginSubmit.
// Original: this in ECX (g_RenderShell), returns AL (go); LtiRenderer_vtbl
// slot 15, called by PgPrimitive_SubmitToGPU (ours calls this directly).
// Entry: game/render_shell.hpp's declaration (hooks/reimpl.cpp).
//
// Present the previous frame (unless g_SuppressPresent), BeginScene, drain
// the GPU (event query + Sleep(0) spin), bind RT0/depth, clear. Calls:
// Dx9_SetRenderTarget, Dx9_SetDepthStencilSurface (the game's state caches);
// the device directly for Present/BeginScene/CreateQuery/Clear and the query.
// Failures go to our log instead of the game's debug printf.
//
// VR behavior inside it, as direct calls:
//   - pacing::on_drain_check_busy at the drain check (0x0074ab5f; a MidHook
//     there while the original ran): the replay pass skips the GPU drain.
//   - msaa::on_begin_submit before the target binds: multisample
//     rasterization on (the engine turns it off with its own AA off).
//
// Writes directly: g_InD3DScene.
#pragma once

#include "game/render_shell.hpp"

namespace mc2vr::reimpl {

bool LtiRenderer_BeginSubmit(game::RenderShell *self);

} // namespace mc2vr::reimpl
