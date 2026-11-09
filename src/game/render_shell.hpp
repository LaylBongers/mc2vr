// RenderShell (the render shell object and its per-view helpers): globals and
// the original functions the reimplemented code calls.
#pragma once

#include <cstdint>

#include "game/function.hpp"
#include "game/global.hpp"

namespace mc2vr::game {

struct RenderShell;  // layout: Ghidra /RenderPath (not needed here yet)

using g_RenderShellPtr = Global<RenderShell *, 0x00dfb2fcu>;  // -> g_RenderShell 0x017ceaf0
using g_CurViewType = Global<uint8_t, 0x01169792u>;
using g_HalfResPassAllowed = Global<uint8_t, 0x01175616u>;
using g_HalfResPassActive = Global<uint8_t, 0x01175617u>;

inline constexpr Function<void(RenderShell *shell, uint32_t slot, uint32_t view)> RenderShell_BindViewTexture{
    "RenderShell_BindViewTexture", 0x00749230u, {EAX, ECX, EDI}};
inline constexpr Function<void()> RenderShell_ViewType9_StretchBlit{"RenderShell_ViewType9_StretchBlit",
                                                                     0x0074adf0u};
inline constexpr Function<void()> RenderShell_BeginHalfResPass{"RenderShell_BeginHalfResPass", 0x0074b3e0u};

} // namespace mc2vr::game
