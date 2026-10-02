# Render Path

Per-address facts (names, chain, vtables, queue layout) are stored in the Ghidra project — plate comment on `GameShell_FrameTick` (`0x00630e10`), the `Analysis/render-path` bookmarks, and `GameStateBase`/`RenderQueue` structs. This file intentionally does not repeat them.

## Threading model (important, easy to get wrong)

- Rendering is NOT threaded. The D3D device is used on the main/window thread only: `RenderSystem_Init` asserts `GetCurrentThreadId() == GetWindowThreadProcessId(hwnd)`, and the window is created on the main thread inside `GameShell_Run`.
- The priority-boosted thread created during boot (`0x008271b0` → `FUN_00827450`) looks like a render thread (event wait, frame counters, Sleep-based pacing) but is the **streaming-IO worker** (`ReadFile` on package files). Don't hook it for rendering.
- The render "queue" (`g_RenderQueue` @ `0x00ff3618`) is a same-thread deferred command list, not a thread handoff: producers publish packets during the frame pipeline, the Dx9 layer interprets them later in the same frame.

## How the path was located (anchors)

- No `EndScene`/`Present` imports; only `Direct3DCreate9` — D3D9 is fully dynamic, every device call is vtable-indirect. Rules out import-xref tracing; worked from the state stack down and `Direct3DCreate9`'s single caller up.
- `Direct3DCreate9` caller = `RenderSystem_Init`, identified by its error string `"ERROR!!! RenderSystem::Init() called from the wrong thread!"` and source paths `"D:\projects\Mercs2_PC\LTI\Src\Dx9_State.cpp"` → render lib is "LTI" (Dx9_State.cpp). Debug-string anchors remain the fastest way to name LTI functions.
- Stack registration found by xrefs writing `g_GameStateStack`; entries read raw, giving all 5 state objects + vtables; top state `[4]`'s update slot leads to the frame pipeline.
- Render consumer found from xrefs to the `GetD3DDevice` thunk — only 3 functions call it directly, one of which is called from the RenderShell frame function.
- Device-lost branch (`DAT_01174a94 == 1`) gave the RenderShell slot semantics for free: `slot01` invalidate → `slot03` timed render → `slot02` restore.

## D3D9 device vtable offsets (observed, confirmed by call signatures)

- `+0xe4` SetRenderState, `+0x10c` SetSamplerState, `+0x114` SetTexture (all verified against arg patterns in `RenderSystem_Init` state apply).
- BeginScene/EndScene/Present/Reset offsets not yet pinned — they live below `RenderCmd_ExecuteStream` in SecuROM-encrypted thunks. Verify at runtime before hooking.

## VR hook strategy (render side)

- Stereo submission point: `RenderQueue_SubmitWorldPackets` walks the view/portal table (`0x012865e0`, stride `0x810`) — extra eye/view submissions belong there (producer side), or duplicate at the consumer via `RenderCmd_ExecuteStream`'s opcode switch.
- Frame-level hook: `RenderShell_RenderFrame` (vtable slot03 path) is where device state is applied per frame; the two no-op `RenderShell` hooks (`vt[4]`/`vt[5]`) remain the low-risk injection slots (see `main_game_loop.md`).
- Camera data: per-view RenderShell sub-objects hang off `g_RenderShellPtr` at `idx*0x3a0`; view/projection copied into packets at submit time — eye matrices must be in place before `RenderQueue_SubmitWorldPackets` runs (it is the LAST call of the frame pipeline).
- All draw state is cached by the Dx9 layer (render-state cache, `0x105` entries, cleared by invalidate slot) — raw `SetRenderState` interception outside the layer will desync its cache.

## Open items

- `thunk_FUN_0256b6f0` (called after every frame, both normal and device-lost branches) — SecuROM-encrypted body; suspected render submit/flip. Needs runtime confirmation.
- Producer counter semantics in `g_RenderQueue` (`countersA`/`countersB`, packed 16-bit pairs; producers increment both halves and spin-wait on `(A+B) % capacity`) — exact consumer-progress tracking not fully derived; re-check at runtime.
- `Present`/`EndScene` call sites (encrypted region below `RenderCmd_ExecuteStream`) — find at runtime by breaking on the device vtable.
- `g_RenderQueue2` (`0x00ff3650`) consumer and purpose — submissions seen from loading-screen path (`FUN_004c9580`), `FUN_00429510`, `FUN_00403720`; likely 2D/overlay queue. Unconfirmed.
- `GameState3_Update` / `GameState2_Frontend_Update` internals — named by position, semantics still unexplored.
- View/portal table layout at `0x012865e0` (stride `0x810`, count `DAT_00d29e60`) — field map not yet extracted; needed before stereo view injection.
- `0x0117527c` adapter remap table / multi-adapter handling in `RenderSystem_Init` — not explored (single-GPU assumption for now).
