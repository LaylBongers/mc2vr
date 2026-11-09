// View-table dump + command histogram (docs/hooks.md hook
// table). Feeds the stereo submission design. Three instrumentations plus a
// queue poller:
//   - MidHook at RenderCmd_ExecuteStream's opcode dispatch -> per-opcode
//     histogram (what actually executes).
//   - MidHook at RenderQueue_SubmitWorldPackets' per-view loop head ->
//     per-frame view aggregation; with debug_view_dumps=on also hex dumps of
//     view-table entries (one per new index + periodic re-dumps). The dumps
//     are written from the game thread — tens of lines each, a visible hitch.
//   - MidHooks at GameShell_FrameTick's two per-frame g_RenderShell slot 5/4
//     call sites (PostUpdateHook / EndOfFrameHook, both the no-op
//     VirtHook_NoOp): frame counters + the IPC event drain.
//   - Poller thread sampling the g_RenderQueue producer counters.
// MidHook handlers run on the game's update/render threads
// (docs/reverse_engineering/render_threading.md); the poller reads their
// counters cross-thread — plain u64 reads, exact values not required.
#pragma once

#include <cstdint>

namespace mc2vr::render {

// Best-effort per component: failures are logged and don't stop the rest
// (game keeps running).
//
// Early: plaintext .text hooks only (opcode/view MidHooks, upload gate,
// stub tracer, SubmitToGPU). Safe before the game's first instruction.
void install_early();

// Post-boot (first Present): the queue poller thread (reads g_RenderQueue
// fields the engine initializes during boot).
void install_late();

// mc2vr.conf debug_view_dumps=on|off (default off): view-table entry dumps.
bool set_view_dumps(const char *value);

// Per executed render command (from the RenderCmd_ExecuteStream
// reimplementation): the opcode histogram and debug_stream_capture's tap.
void on_render_command(const uint32_t *cmd, const uint32_t *stream, uint32_t arg2, uint32_t arg3);

} // namespace mc2vr::render
