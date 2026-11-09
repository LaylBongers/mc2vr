// The game's HUD drawing called from RenderShell_EndSubView
// (docs/reverse_engineering/gui_renderer.md; Ghidra plates). These emit
// ordinary draw records during the scene build.
#pragma once

#include <cstdint>

#include "game/function.hpp"
#include "game/global.hpp"

namespace mc2vr::game {

using DAT_01174cdc = Global<uint8_t *, 0x01174cdcu>;  // +0x63 u8: the HUD draws this frame
using DAT_01174fb0 = Global<uint8_t *, 0x01174fb0u>;  // +0x6c: the world-marker / HUD context; the GUI's ESI input
constexpr uintptr_t kHudEnabledFlag = 0x63;
constexpr uintptr_t kHudContext = 0x6c;

// The depth-tested world icons (vehicle entry, pickups, objectives).
inline constexpr Function<void(void *ctx, uint32_t view)> Hud_DrawWorldMarkers{"Hud_DrawWorldMarkers", 0x0060f610u,
                                                                               {STK, STK}};
inline constexpr Function<void(void *ctx, uint32_t view)> FUN_0060fe20{"FUN_0060fe20", 0x0060fe20u, {STK, STK}};
inline constexpr Function<void(uint32_t view)> FUN_00709890{"FUN_00709890", 0x00709890u, {STK}};
// The GUI (-> mrxgui manager -> Scaleform).
inline constexpr Function<void(uint8_t *esi, uint32_t view)> FUN_00609d80{"FUN_00609d80", 0x00609d80u, {ESI, STK}};

// Hud_DrawQuad (0x00617c70) draws every world-marker quad. It is not
// reimplemented: its material check is a SecuROM VM thunk (0x006188f0 ->
// 0x0255e420) that crashed when called from our code (presumably it reads its
// caller's frame). hooks/hud.cpp observes it instead.

} // namespace mc2vr::game
