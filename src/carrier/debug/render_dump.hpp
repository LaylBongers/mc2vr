// View-table dump + command histogram (docs/hooks.md hook
// table). Feeds the stereo submission design. Three instrumentations plus a
// queue poller:
//   - MidHook at RenderCmd_ExecuteStream's opcode dispatch -> per-opcode
//     histogram (what actually executes).
//   - MidHook at RenderQueue_SubmitWorldPackets' per-view loop head ->
//     per-frame view aggregation + one-shot hex dumps of view-table entries
//     (the missing field map).
//   - MidHooks at GameShell_FrameTick's two per-frame g_RenderShell slot 5/4
//     call sites (PostUpdateHook / EndOfFrameHook, both the no-op
//     VirtHook_NoOp): frame counters + the IPC event drain. Replaced the
//     cloned-vtable claim 2026-11-08 (no vptr dependency).
//   - Poller thread sampling the g_RenderQueue producer counters.
// MidHook handlers run on the main thread (single-threaded render path);
// the poller reads their counters cross-thread — plain u64 reads, exact
// values not required for diagnostics.
#pragma once

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

} // namespace mc2vr::render
