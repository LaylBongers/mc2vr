// RenderCmd_ExecuteStream (0x008569d0) — reimplementation.
//
// STATUS: live; shadow-verified, no frame cost (render_cmd.md). Ghidra plate:
// RenderCmd_ExecuteStream. Note: docs/reverse_engineering/render_cmd.md.
// Original: cdecl, called only by PgPrimitive_SubmitToGPU (0x008562be).
// Calls: game/dx9.hpp, game/render_cmd.hpp, game/render_shell.hpp functions;
// op 23 calls the original VM thunk.
//
// Replaces the original: hooks/reimpl.cpp. Writes g_LastExecuteStream,
// g_ClipPlaneEnableMask, g_RenderCmdPassObject, g_HalfResPassAllowed and
// g_HalfResPassActive (debug/shadow/render_cmd.cpp snapshots exactly these).
#pragma once

#include <cstdint>

namespace mc2vr::reimpl {

void RenderCmd_ExecuteStream(const uint32_t *stream, void *vs_ctx, void *ps_ctx);

} // namespace mc2vr::reimpl
