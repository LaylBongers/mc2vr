# Render Path

Per-address facts (names, chain, vtables, queue layout) are stored in the Ghidra project — plate comments on `GameShell_FrameTick` (`0x00630e10`), `RenderShell_RenderFrame` (`0x00855690`), `LtiRenderer_vtbl` (`0x00bd38e8`), `RenderQueue_SubmitWorldPackets` (`0x0048e620`), the `g_LtiRenderer` plate (`0x01175288`, Dx9 wrapper slot map), and the `GameStateBase`/`RenderQueue`/`LtiRenderer`/`Dx9StateWrapper` structs. This file intentionally does not repeat them.

## Threading model (important, easy to get wrong)

- Rendering is NOT threaded. The D3D device is used on the main/window thread only: `RenderSystem_Init` asserts `GetCurrentThreadId() == GetWindowThreadProcessId(hwnd)`, and the window is created on the main thread inside `GameShell_Run`.
- The priority-boosted thread created during boot (`0x008271b0` → `FUN_00827450`) looks like a render thread (event wait, frame counters, Sleep-based pacing) but is the **streaming-IO worker** (`ReadFile` on package files). Don't hook it for rendering.
- The render "queue" (`g_RenderQueue` @ `0x00ff3618`) is a same-thread deferred command list, not a thread handoff: producers publish packet elements during the frame pipeline; the interpreter (S0: SecuROM-VM'd, entered via the `0x0050f660` stub right after `SubmitWorldPackets`) turns them into draw-records + command streams consumed later in the same frame by `RenderFrame`/`ExecuteStream`. The producer's in-publish spin-wait can never deadlock precisely because consumer and producer are the same thread.

## How the path was located (methodology anchors)

- No `EndScene`/`Present` imports; only `Direct3DCreate9` — D3D9 fully dynamic, every device call vtable-indirect. Import-xref tracing useless; worked from the state stack down and `Direct3DCreate9`'s single caller up.
- Debug-string anchors (source paths like `"D:\projects\Mercs2_PC\LTI\Src\..."`) remain the fastest way to name LTI/Pangea functions — e.g. `RenderSystem_Init` via its wrong-thread assert, `LtiRenderer_BeginSubmit`/`EndSubmit` via "BeginSubmit() called when already in a scene!".
- Stack registration found by xrefs writing `g_GameStateStack`; top state `[4]`'s update slot leads to the frame pipeline.
- Render consumer found from xrefs to the `GetD3DDevice` thunk (`0x0047f2f0`, 12 refs); one caller sits in the RenderShell frame path.
- Device-lost branch (`DAT_01174a94 == 1`) gave the RenderShell slot semantics: `slot01` invalidate → `slot03` timed render → `slot02` restore.
- Runtime caller-attribution trick (carrier): `__builtin_return_address(0)` + module lookup in hook bursts, or a MidHook at entry reading `ECX`/`[ESP]` for thiscall/virtual sites.

## Frame driver chain (runtime-confirmed via carrier hooks, M2/M2.5; S0 static additions 2026-10-02)

```
InGameShellState_FramePipeline (producer side, no D3D):
  ... RenderPackets_ResetViewFlags, RenderQueue_SubmitWorldPackets (0x0048e620: walks the
  ACTIVE-view linked list, stages one 96-byte {size,ptr} element per type-2/4 view, bulk-
  publishes to g_RenderQueue) ...
  0x004c99f9 -> SecuROM VM stub 0x0050f660: SUSPECTED packet interpreter (S0: no plaintext
  code reads the ring / frame-ctx 0xd29xx fields; it builds the draw-records + command
  streams consumed below)
RenderShell_RenderFrame (0x00855690, entered via vtable slot03 = RenderFrameTimed 0x0085abd0)
  RenderShell_InitDrawRecordTables (0x00853ee0, called from LtiRenderer-side FUN_007494e0)
  zeroes/allocates the draw-record tables + pools; RenderFrame then walks the 0x58-records
  (head g_DrawRecordHead, base g_DrawRecordBase) — VM-filled — applying per-record state:
  +0xC2 (0x00855752): virtual dispatch on g_RenderShell (0x017ceaf0, holds BASE LtiRenderer_vtbl 0x00bd38e8)
    slot 15 LtiRenderer_BeginSubmit (0x0074aaa0): [if g_SuppressPresent==0] Present(prev) via
        LtiRenderer_Dx9_Present (0x00748fb0: device vtable slot 17, all-NULL args); BeginScene (slot 41);
        sets in-scene flag DAT_0117526c=1
    ... per record: RenderCmd_ExecuteStream (0x008569d0) runs the record's dword command
        stream (record+0x2c) — 27-opcode interpreter, device calls via Dx9_State layer ...
    slot 22 LtiRenderer_EndSubmit (0x0074ac30): clears state-cache flags, EndScene (slot 42), clears flag
```

- `g_RenderShell` holds the **base** `LtiRenderer_vtbl` at frame time; the derived `RenderShell_vtbl` (0x00be84c0) overrides (incl. slot 15 `Flush`) never run. Type as `LtiRenderer`/`LtiRenderer_vtbl` (done in Ghidra).
- Calls per frame: Present/BeginScene/EndScene exactly 1:1 with `GameShell_FrameTick` count. `Reset` (slot 16) only on present-param change (resolution etc.), carries the new `D3DPRESENT_PARAMETERS`.
- Present args always all-NULL. Menu present params: 2560x1440, fullscreen, DISCARD, 60 Hz, interval DEFAULT, `hDeviceWindow=0x100b6`.
- The old "Begin/EndScene + Present live below this in SecuROM-encrypted thunks" note is FALSIFIED — they live in plaintext LtiRenderer_* functions; the producer/camera/draw-data chain (`SubmitWorldPackets` loop body, element staging, `RenderFrame` record walk) is also fully plaintext. What IS VM-protected (S0): the packet interpreter between the ring and the draw-records.

## D3D9 device vtable offsets

Header layout validated by the anchor: game applies render state via `+0xe4` = slot 57 = SetRenderState per d3d9.h (SetDialogBoxMode quirk at slot 20 included). Corrected labels: `+0x10c` = slot 67 SetTextureStageState (36-state per-stage loops, stride-`0x24` cache), `+0x114` = slot 69 SetSamplerState (4-arg `(0, MAGFILTER, LINEAR)`), SetTexture = slot 65. Pinned hook slots: Reset 16, Present 17, BeginScene 41, EndScene 42; swapchain GetSwapChain 14, GetPresentParameters 9. S1 will add: SetTransform 44 (`+0xB0`), SetViewport 47 (`+0xBC`).

## S2 sweep (2026-10-02): RenderFrame internals — per-address facts in Ghidra

Navigate from the named symbols (all plate-commented). Only the non-obvious rules:
- Dx9 state wrapper: every D3D call in the render path goes through `g_LtiRenderer->dx9State` (`+0x5bc`; global `0x01175288` typed `LtiRenderer *`). Slot map = `Dx9StateWrapper_vtbl` struct members + `g_LtiRenderer` plate. Rules: wrapper vtable = IDirect3DDevice9 order minus ONE method in device slots 70..81 (wrapper slot n == device slot n+1 for n >= 81); dirty-tracking caches are caller-side in the `Dx9_*` functions — hook the wrapper layer, never the raw device (raw hooks desync `g_RenderStateCache` and the texture/sampler/RT caches).
- Draw-record list: 0x58 records (`g_DrawRecordBase`/`g_DrawRecordHead`); 6-byte sort key + ushort next-link table at `0x01153700` (plates there). `PgPrimitive_SortList` (`0x00854c10`) radix-sorts them with NO plaintext callers — the VM'd record builder also orders the list. Per-record apply order: plate on `RenderShell_RenderFrame`. Dirty-check cache block `0x01169788` (layout plate there). View types: plate on `RenderShell_SetViewType`.
- Precache: plates on `RenderShell_PrecacheLoadStep`/`RenderShell_PrecacheFinish`; `g_SuppressPresent` suppresses Present during precache frames.
- `Lti_LazyNameHash` (`0x008244a0`, 139 callers): per-site FNV-1a "Class::Method" IDs cached in `.bss`, read only by VM'd code — inert telemetry, NOT feature/device checks.
- `LtiRenderer_EndSubmit` StretchRects RT0 → backbuffer (`LtiRenderer+0x3ea4`) whenever they differ — existing RT→backbuffer seam for the S4 compositor (also noted in `docs/stereo_design.md`).

## VR hook strategy (render side)

- Stereo submission point: `RenderQueue_SubmitWorldPackets` walks the ACTIVE views — an intrusive linked list (S0: head = `DAT_00d29e60`, an INDEX; link = `ViewEntry+0x4`; negative terminates) — and stages one 96-byte element per type-2/4 view: three `{byte-size, ptr}` pairs — {`0x30` camera staging `this+0xc2110+idx*0x30` (inline pos/serial/rot copy, site `0x0048ec3e`)}, {`0x810` live `ViewEntry*`}, {`0x680` frame-ctx block `this+0xd2950`}. Deref is at CONSUME time, post-walk, inside SecuROM-VM'd code — per-eye data must therefore be shadow COPIES referenced by a cloned element (staging site `0x0048ef71`, count `[ESP+0x19a00]`, array `[ESP+0x79a0]`, cap 768). See `docs/stereo_design.md` S0/S3.
- Frame-level slots: `g_RenderShell` vtable slots 4/5 (`EndOfFrameHook`/`PostUpdateHook`, +0x10/+0x14) are `VirtHook_NoOp` on the live base vtable — claim via cloned-vtable swap on `g_RenderShell` (proven M3, 1:1 with frames).
- Camera data: per-view RenderShell sub-objects hang off `g_RenderShellPtr` at `idx*0x3a0`; they are copied once per FRAME into the frame-ctx 0x680 block (`this+0xd2950+0xEC`, S0) — not per view, not eye slots. The per-view camera handed to the consumer is `{0x810, ViewEntry*}` by POINTER (derefed post-walk) plus the inline `0x30` staging copy — eye matrices must be in place as shadow copies before the consumer runs, never patched in-place between passes.
- All draw state is cached by the Dx9 wrapper layer — caller-side caches in the `Dx9_*` functions (`g_RenderStateCache`, `0x105` entries, cleared by the invalidate slot; plus texture/sampler/RT caches). Raw device-call interception outside the wrapper desyncs them; hook the `Dx9StateWrapper` vtable slots or the `Dx9_*` functions instead.

## Open items

- **Wrapper vtable ADDRESS (S2, type annotation DONE)**: `Dx9StateWrapper_vtbl` / `Dx9StateWrapper` structs exist in Ghidra and `g_LtiRenderer` is typed — all known slots decompile as named methods. What remains is the vtable's runtime-installed ADDRESS (ctor unfound — likely in the 0x00749xxx LtiRenderer region, cf. `FUN_007494e0` which calls `RenderShell_InitDrawRecordTables`). Finding it would let the struct be applied at the static vtable data, xref'ing ~100 remaining `slotNN` members and confirming the one omitted device method (slots 70..81).
- `0x0117560c` — precache gate flag (if 0 while precache requested, frame bails early); semantics unexplored.
- `DAT_017d1818` / `DAT_017d2a94` — vtable'd singletons used by the type-8/9 passes (surface/rect providers); unidentified.
- The two frame-preamble hash keys (`0x5e84ea6d`, `0x16085a8d`, both with low dword `0xf011157a`) — presumed the two named world views; reverse the name strings if a registry-writer is found.

- Producer counter semantics in `g_RenderQueue` — **RESOLVED (S0)**: two packed 16-bit halves at +0x10 (countersA) and +0x14 (countersB). Producers (single-element and bulk paths in `SubmitWorldPackets`, plus the other nine producers) do: `countersB.low += count`, `countersA.high += count`, spin-wait until the consumer-derived position `(countersA.low + countersA.high) % capacity` matches the producer slot `(countersB.low + countersA.low) % capacity`. `countersA.low` is advanced only by the CONSUMER — which is SecuROM-VM'd (no plaintext writer), explaining M3's "queue+0x10 wraps like a ring position" and "+0x14 stayed 0" observations. Old packed-pair spin model: close, but the halves' roles were swapped.
- `g_RenderQueue2` (`0x00ff3650`) consumer — **narrowed (S0)**: its pointers sit inside the same 0x680 frame-ctx block handed per element; the VM interpreter at `0x0050f660` (call site `0x004c99f9`) is the prime suspect for consuming BOTH queues. S1 brackets it with MidHooks and counters deltas.
- View/portal table walk — **RESOLVED (S0)**: intrusive linked list; `DAT_00d29e60` = head INDEX (M3 "registered-view count" label wrong; also stored to frame-ctx `+0xd2a10`), link `ViewEntry+0x4`, negative terminates. Per-view element format, camera staging sites, and the 768-element staging cap are on the `SubmitWorldPackets` plate comment.
- `GameState3_Update` / `GameState2_Frontend_Update` internals — named by position, semantics unexplored.
- `0x0117527c` adapter remap table / multi-adapter handling in `RenderSystem_Init` — not explored (single-GPU assumption).
- `vt[4]`/`vt[5]`: ~~confirm anything actually CALLS them~~ **RESOLVED (M3)**: both slots called exactly once per frame by `GameShell_FrameTick` (claimable, mechanism proven via cloned vtable; survived alt-tabs/cutscene/mission load).
