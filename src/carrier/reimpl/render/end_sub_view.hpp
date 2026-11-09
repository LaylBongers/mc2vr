// RenderShell_EndSubView (0x0062ee20) — reimplementation.
//
// STATUS: live; not yet verified against the original.
// Ghidra plate: RenderShell_EndSubView.
// Original: EAX = phase, EDI = sub-view; reached from
// RenderShell_BuildSceneRecords' listener (via a VM-slot jmp), build thread.
// Entry: game/render_shell.hpp's declaration (hooks/reimpl.cpp).
//
// For phase 2 only, in order: the post chain and composite
// (PostFx_ChainAndComposite), the world markers (Hud_DrawWorldMarkers),
// renderer slot 14, FUN_0060fe20, FUN_00709890, the GUI (FUN_00609d80). All
// emit ordinary draw records. Calls: game/render_shell.hpp, game/hud.hpp.
//
// VR behavior inside it, as direct calls:
//   - vr/hud brackets the world-marker and the HUD record ranges (by the
//     engine's record write count), so the record walk can draw the HUD once
//     into its own layer (docs/hud.md).
//
// Writes directly: nothing.
#pragma once

#include <cstdint>

namespace mc2vr::reimpl {

void RenderShell_EndSubView(uint32_t phase, uint32_t view);

} // namespace mc2vr::reimpl
