# View table, staging and render queue

Per-address facts on the Ghidra plates (`ViewEntry` struct, `RenderQueue_SubmitWorldPackets`, `g_RenderQueue`, `g_RenderQueue2`). Frame chain: [frame_chain.md](frame_chain.md). Who actually reads this data (watch-proven): [camera_data_flow.md](camera_data_flow.md).

## ViewEntry (Ghidra struct, applied at `g_ViewTable` `0x012865e0`)

Load-time entries are template/zero. Camera data: three `ViewMatrixSlot`s (stride 0xc0) at +0x020 (per slot: mtx[0] viewToWorld w/ camera pos, mtx[1] worldToView w/ negated pos, then pos/dir/kind/params), FOV half-angle sin/cos at +0x2ec/+0x2f4, pose-store handle key at +0x010, camera position copies at +0x7ac (previous) / +0x7c4 (current), quaternion at +0x7d4, near-plane-ish params near +0x188 (5 normal / 20 satellite-style / 9.81 water view), `ViewRef` pointer +0x7e4 (struct `ViewRef`, vtable `0x00bac1c8` = `ViewRef_vtbl`), +0x7e8 = viewRef+0x20, +0x7ec upstream camera object, handle-id mirror at +0x800.

- Active views form an intrusive linked list (head `DAT_00d29e60` is an INDEX, link `ViewEntry+0x4`, negative terminates; also stored to frame-ctx `+0xd2a10`; an earlier "registered-view count" label was wrong). Per-frame submissions: gameplay tens, special cameras (satellite designation) up to 608, indices to 239 (~256-entry table). Type-4 views: never observed in any scenario.
- View indices are NOT stable across runs — pin only with the logged idx of that run. Liveness marker: companion byte `g_ViewTable3 + idx*0x20 + 0x18` (t3) is 00 for the 12 loading templates (shared obj ptr 0x1f758960) and 01 for persistent live views.

## Staging (producer side)

`RenderQueue_SubmitWorldPackets` walks the ACTIVE views and stages one 96-byte element per type-2/4 view: three `{byte-size, ptr}` pairs — {`0x30` camera staging `this+0xc2110+idx*0x30` (inline pos/serial/rot copy, site `0x0048ec3e`)}, {`0x810` live `ViewEntry*`}, {`0x680` frame-ctx block `this+0xd2950`}. Deref is at CONSUME time, post-walk, inside SecuROM-VM'd code (staging site `0x0048ef71`, count `[ESP+0x19a00]`, array `[ESP+0x79a0]`, cap 768; iterator reload `0x0048f013`, back-edge `0x0048f01f` — mapped; hooks are not needed for the GPU-boundary design).

- Per-view RenderShell sub-objects hang off `g_RenderShellPtr` at `idx*0x3a0`; they are copied once per FRAME into the frame-ctx 0x680 block (`this+0xd2950+0xEC`, static decode) — not per view, not eye slots.
- The VM'd consumer demonstrably WRITES the staged camera block back (walk-tail copy-back `0x0048F72D`): the staging is a ROUND-TRIP — see [camera_data_flow.md](camera_data_flow.md).

## Render queue

- `g_RenderQueue` ring (`0x00ff3618`): elem 96, cap 4096, position = `+0x10` low16 % cap. `g_RenderQueue2` (`0x00ff3650`) carries 2D/overlay submissions.
- Counter semantics — **RUNTIME-RESOLVED (HUD measurement, 2026-10-06, live-verified via `hud2 raw` bursts)**: `+0x10` high16 = pending-unconsumed element count (producers `+=` during the frame — visible nonzero at SubmitToGPU entry; the VM consumer CLEARS it and advances low16 between SubmitToGPU entry and BeginSubmit's Present); `+0x10` low16 = ring position (moves on publish/consume; FROZEN during both pass walks and the inter-pass gap — `p1=b2=p2=b0` in every sampled frame); `+0x14` NEVER moves at runtime (unused by the live path — the static model's `countersB.low += count` producer step does not manifest). The static packed-halves model was close on the halves' roles but wrong on their meaning and the consume timing.
- Consumption point: BOTH queues are fully consumed BEFORE pass 1 begins drawing (inside pass-1 SubmitToGPU, pre-BeginSubmit) — the VM interpreter's consume effect lands inside the pass-1 submit window, and HUD/2D records therefore exist in the record table both passes walk (HUD conclusion: one-eye HUD impossible; [../stereo.md](../stereo.md)).
- `g_RenderQueue2` consumer — narrowed (static analysis): its pointers sit inside the same 0x680 frame-ctx block handed per element; the VM interpreter at `0x0050f660` (call site `0x004c99f9`) is the prime suspect for consuming BOTH queues.

## Command streams

Histogram: gameplay ~21 opcodes (menu 9), 1.5k–3.4k cmds/frame; bins 00/01/02/03 dominate; 27 opcodes defined. Streams carry no draws (op 0x0f = Clear). Full opcode table and interpreter facts on the `RenderCmd_ExecuteStream` plate (dedupe global `0x011697b8`; op 0x13 is a 2-dword no-op; op 0x02/0x03 constant uploads carry count in EDX; op 0x08 screen-constant refresh is viewport/view-dependent).

- Gameplay's final composite is a single DRAW into RT0=backbuffer (UpdateSurface/UpdateTexture never fire); menus/loading use a mainRT→backbuffer StretchRect. The tonemapped LDR finals are on the backbuffer at the end of each pass.

## Frame-level slots

`g_RenderShell` vtable slots 4/5 (`EndOfFrameHook`/`PostUpdateHook`, +0x10/+0x14) are `VirtHook_NoOp` on the live base vtable — claimable via cloned-vtable swap on `g_RenderShell` (both slots called exactly once per frame by `GameShell_FrameTick`, proven via cloned vtable through alt-tabs/cutscene/mission load; [../hooks.md](../hooks.md)).
