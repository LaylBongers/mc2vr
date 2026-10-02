# Render Path

Per-address facts (names, chain, vtables, queue layout) are stored in the Ghidra project — plate comments on `GameShell_FrameTick` (`0x00630e10`), `RenderShell_RenderFrame` (`0x00855690`), `LtiRenderer_vtbl` (`0x00bd38e8`), the `Analysis/render-path` bookmarks, and the `GameStateBase`/`RenderQueue`/`LtiRenderer` structs. This file intentionally does not repeat them.

## Threading model (important, easy to get wrong)

- Rendering is NOT threaded. The D3D device is used on the main/window thread only: `RenderSystem_Init` asserts `GetCurrentThreadId() == GetWindowThreadProcessId(hwnd)`, and the window is created on the main thread inside `GameShell_Run`.
- The priority-boosted thread created during boot (`0x008271b0` → `FUN_00827450`) looks like a render thread (event wait, frame counters, Sleep-based pacing) but is the **streaming-IO worker** (`ReadFile` on package files). Don't hook it for rendering.
- The render "queue" (`g_RenderQueue` @ `0x00ff3618`) is a same-thread deferred command list, not a thread handoff: producers publish packets during the frame pipeline, the Dx9 layer interprets them later in the same frame.

## How the path was located (methodology anchors)

- No `EndScene`/`Present` imports; only `Direct3DCreate9` — D3D9 fully dynamic, every device call vtable-indirect. Import-xref tracing useless; worked from the state stack down and `Direct3DCreate9`'s single caller up.
- Debug-string anchors (source paths like `"D:\projects\Mercs2_PC\LTI\Src\..."`) remain the fastest way to name LTI/Pangea functions — e.g. `RenderSystem_Init` via its wrong-thread assert, `LtiRenderer_BeginSubmit`/`EndSubmit` via "BeginSubmit() called when already in a scene!".
- Stack registration found by xrefs writing `g_GameStateStack`; top state `[4]`'s update slot leads to the frame pipeline.
- Render consumer found from xrefs to the `GetD3DDevice` thunk (`0x0047f2f0`, 12 refs); one caller sits in the RenderShell frame path.
- Device-lost branch (`DAT_01174a94 == 1`) gave the RenderShell slot semantics: `slot01` invalidate → `slot03` timed render → `slot02` restore.
- Runtime caller-attribution trick (carrier): `__builtin_return_address(0)` + module lookup in hook bursts, or a MidHook at entry reading `ECX`/`[ESP]` for thiscall/virtual sites.

## Frame driver chain (runtime-confirmed via carrier hooks, M2/M2.5)

Entirely plaintext — no SecuROM involvement in frame submission (`thunk_FUN_0256b6f0` falsified as submit/flip; deprioritize).

```
RenderShell_RenderFrame (0x00855690, entered via vtable slot03 = RenderFrameTimed 0x0085abd0)
  +0xC2 (0x00855752): virtual dispatch on g_RenderShell (0x017ceaf0, holds BASE LtiRenderer_vtbl 0x00bd38e8)
    slot 15 LtiRenderer_BeginSubmit (0x0074aaa0): [if DAT_011755c3==0] Present(prev) via
        LtiRenderer_Dx9_Present (0x00748fb0: device vtable slot 17, all-NULL args); BeginScene (slot 41);
        sets in-scene flag DAT_0117526c=1
    ... RenderCmd_ExecuteStream (0x008569d0) consumes queued packets (device calls via Dx9_State layer) ...
    slot 22 LtiRenderer_EndSubmit (0x0074ac30): clears state-cache flags, EndScene (slot 42), clears flag
```

- `g_RenderShell` holds the **base** `LtiRenderer_vtbl` at frame time; the derived `RenderShell_vtbl` (0x00be84c0) overrides (incl. slot 15 `Flush`) never run. Type as `LtiRenderer`/`LtiRenderer_vtbl` (done in Ghidra).
- Calls per frame: Present/BeginScene/EndScene exactly 1:1 with `GameShell_FrameTick` count. `Reset` (slot 16) only on present-param change (resolution etc.), carries the new `D3DPRESENT_PARAMETERS`.
- Present args always all-NULL. Menu present params: 2560x1440, fullscreen, DISCARD, 60 Hz, interval DEFAULT, `hDeviceWindow=0x100b6`.

## D3D9 device vtable offsets

Header layout validated by the anchor: game applies render state via `+0xe4` = slot 57 = SetRenderState per d3d9.h (SetDialogBoxMode quirk at slot 20 included). Corrected labels: `+0x10c` = slot 67 SetTextureStageState (36-state per-stage loops, stride-`0x24` cache), `+0x114` = slot 69 SetSamplerState (4-arg `(0, MAGFILTER, LINEAR)`), SetTexture = slot 65. Pinned hook slots: Reset 16, Present 17, BeginScene 41, EndScene 42; swapchain GetSwapChain 14, GetPresentParameters 9.

## VR hook strategy (render side)

- Stereo submission point: `RenderQueue_SubmitWorldPackets` walks the view/portal table (`0x012865e0`, stride `0x810`) — extra eye/view submissions belong there (producer side), or duplicate at the consumer via `RenderCmd_ExecuteStream`'s opcode switch.
- Frame-level slots: `g_RenderShell` vtable slots 4/5 (`EndOfFrameHook`/`PostUpdateHook`, +0x10/+0x14) are `VirtHook_NoOp` on the live base vtable — claim via cloned-vtable swap on `g_RenderShell` (see launcher_plan.md hook table; runtime callability check is an M3 item).
- Camera data: per-view RenderShell sub-objects hang off `g_RenderShellPtr` at `idx*0x3a0`; view/projection copied into packets at submit time — eye matrices must be in place before `RenderQueue_SubmitWorldPackets` runs (it is the LAST call of the frame pipeline).
- All draw state is cached by the Dx9 layer (render-state cache, `0x105` entries, cleared by invalidate slot) — raw `SetRenderState` interception outside the layer will desync its cache.

## Open items

- Producer counter semantics in `g_RenderQueue` — **partially resolved (M3 runtime)**: field at queue+0x10 is a RING POSITION (values wrap within 0..capacity-1, non-monotonic; capacity 4096, elementSize 96 in this install), not a cumulative producer counter; queue+0x14 stayed 0 during gameplay (read position lives elsewhere or is unused on this path). The old "producer counters, packed 16-bit pairs, spin-wait on (A+B) % capacity" model needs re-derivation against these observations.
- `g_RenderQueue2` (`0x00ff3650`) consumer and purpose — submissions seen from loading-screen path (`FUN_004c9580`), `FUN_00429510`, `FUN_00403720`; likely 2D/overlay queue. Unconfirmed.
- `GameState3_Update` / `GameState2_Frontend_Update` internals — named by position, semantics unexplored.
- View/portal table layout at `0x012865e0` — **first-pass field map derived from M3 runtime dumps** (see `launcher_plan.md` M3 entry; position floats at entry+0x7c4, ViewRef pointers at entry+0x7e4; entries mostly template — steady-state re-dumps pending). Remaining: steady-state entry contents, type-4 view anatomy, camera-matrix fields in the `g_RenderShellPtr+idx*0x3a0` sub-objects.
- `0x0117527c` adapter remap table / multi-adapter handling in `RenderSystem_Init` — not explored (single-GPU assumption).
- `vt[4]`/`vt[5]`: ~~confirm anything actually CALLS them~~ **RESOLVED (M3)**: both slots called exactly once per frame by `GameShell_FrameTick` (claimable, mechanism proven via cloned vtable; survived alt-tabs/cutscene/mission load).
