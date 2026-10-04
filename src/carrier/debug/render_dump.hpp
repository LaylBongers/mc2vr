// M3: view-table dump + command histogram (docs/launcher_plan.md hook
// table). Feeds the stereo submission design. Three instrumentations plus a
// queue poller:
//   - MidHook at RenderCmd_ExecuteStream's opcode dispatch -> per-opcode
//     histogram (what actually executes).
//   - MidHook at RenderQueue_SubmitWorldPackets' per-view loop head ->
//     per-frame view aggregation + one-shot hex dumps of view-table entries
//     (the missing field map).
//   - VmtHook (cloned vtable) on g_RenderShell claiming slots 4/5 (the NoOp
//     EndOfFrameHook/PostUpdateHook) — proves both that the slots are called
//     and that the M4 claim mechanism works. Handlers are new no-ops that
//     count; the original is VirtHook_NoOp (empty), so no original call is
//     needed and the convention (thiscall, no stack args) can't mismatch.
//   - Poller thread sampling the g_RenderQueue producer counters.
// MidHook handlers run on the main thread (single-threaded render path);
// the poller reads their counters cross-thread — plain u64 reads, exact
// values not required for diagnostics.
#pragma once

namespace mc2vr::render {

// Installs everything. Best-effort per component: failures are logged and
// don't stop the rest (game keeps running).
void install();

} // namespace mc2vr::render
