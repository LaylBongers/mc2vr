# Render Path

Per-address facts (names, chain, vtables, queue layout) are stored in the Ghidra project — plate comments on `GameShell_FrameTick` (`0x00630e10`), `PgPrimitive_SubmitToGPU` (`0x00855690`), `LtiRenderer_vtbl` (`0x00bd38e8`), `RenderQueue_SubmitWorldPackets` (`0x0048e620`), the `g_LtiRenderer` plate (`0x01175288`, Dx9 wrapper slot map), the `g_MaterialTable` plate (`0x00ff36f4`, `PgMaterial` layout — formerly mislabeled `g_CameraTable`/`CameraEntry`; the entries are named Pg materials, not cameras), and the `GameStateBase`/`RenderQueue`/`LtiRenderer`/`Dx9StateWrapper`/`PgMaterial` structs. This file intentionally does not repeat them.

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

**Diagram:** `docs/render_diagram.svg` — frame flow, the opaque VM stub and its callbacks, and every MC2VR hook point (installed / optional / planned). Keep it in sync when hooks change.

## Frame driver chain (runtime-confirmed via carrier hooks, M2/M2.5; S0 static additions 2026-10-02)

```
InGameShellState_FramePipeline (producer side, no D3D):
  ... RenderPackets_ResetViewFlags, RenderQueue_SubmitWorldPackets (0x0048e620: walks the
  ACTIVE-view linked list, stages one 96-byte {size,ptr} element per type-2/4 view, bulk-
  publishes to g_RenderQueue) ...
  0x004c99f9 -> SecuROM VM stub 0x0050f660: SUSPECTED packet interpreter (S0: no plaintext
  code reads the ring / frame-ctx 0xd29xx fields; it builds the draw-records + command
  streams consumed below)
PgPrimitive_SubmitToGPU (0x00855690, entered via vtable slot03 = RenderFrameTimed 0x0085abd0)
  RenderShell_InitDrawRecordTables (0x00853ee0, called from LtiRenderer-side FUN_007494e0)
  zeroes/allocates the draw-record tables + pools. SubmitToGPU walks the 0x58 PgPrimitive
  records (head g_PrimitiveHead, base g_PrimitiveBase, link g_PrimitiveNext), applying
  per-record state (order: view type/RT, technique + VS/PS constants, indices, pass object,
  stencil/RT mask, command stream, stream binding, draw — see the plate on the function):
  +0xC2 (0x00855752): virtual dispatch on g_RenderShell (0x017ceaf0, holds BASE LtiRenderer_vtbl 0x00bd38e8)
    slot 15 LtiRenderer_BeginSubmit (0x0074aaa0): [if g_SuppressPresent==0] Present(prev) via
        LtiRenderer_Dx9_Present (0x00748fb0: device vtable slot 17, all-NULL args); BeginScene (slot 41);
        sets in-scene flag DAT_0117526c=1; then a GPU SYNC: an event query (raw device slot 118
        CreateQuery, type 8) is issued and spun on with Sleep(0) until the GPU drains, then
        SetRenderTarget(0)/depth + Clear — every frame waits for all prior GPU work
    ... per record: RenderCmd_ExecuteStream (0x008569d0) runs the record's dword command
        stream (record+0x2c) — 27-opcode interpreter, device calls via Dx9_State layer ...
    slot 22 LtiRenderer_EndSubmit (0x0074ac30): clears state-cache flags, EndScene (slot 42), clears flag
```

## VM stub callbacks (does the opaque stub call plaintext?)

Static evidence: `PoseStore_GetPoseByHandle` has one caller inside `.securom` (`0x03320064`, in `FUN_03320000`, reached via the plaintext thunk `thunk_FUN_03320000` `0x006b52d0` from gameplay code `FUN_006f7d90`) — protected routines can contain native calls into plaintext. Whether the render-path stub (`0x004c99f9` -> `0x0050f660`) does is answered by the tracer in `src/carrier/debug/stub_trace.cpp` (`mc2vr.conf debug_stub_trace=on`): MidHooks bracket the stub call (phases PRE / STUB / POST) and the entry of ~15 plaintext helpers (pose store, view/camera records, draw-record init, technique/shader constant resolvers); each hit logs phase + caller return address + region (`.text` / `Stext`/`.securom`/... / other). Log lines: `trace: <fn> called from <addr> [<region>] phase=<..>` (one-shot per distinct caller) and a 10s `trace: window` table. A hit with phase STUB and a non-`.text` caller = the stub (or protected code it runs) called plaintext directly. **Results (run 2026-10-03, ~20 ten-second windows, ~600 stub calls each):**
- Bracket validity: stub calls == returns in every window, so STUB-phase attribution is reliable. Steady gameplay: `PgPrimitive_SubmitToGPU` runs POST-stub (~574/588 per window); the PRE-heavy windows were loading/transitions.
- **No direct protected-region callback during the stub**: `protected-in-STUB` = 0 for every hooked helper. The pose-store accessors, `Pose_Copy`, `PgMaterial_ctor` etc. have 0 STUB-phase hits.
- **Plaintext IS called during the stub window**: `PoseStore_ResolveHandle` (~14.7k STUB hits total) from three callers — `0x0050c131` (inside the large mutated function `FUN_0050c106`, reached via thunks `FUN_00504a95`/`FUN_0050c0f0`), `0x00649687` (ResolveHandle's own recursion), and `0x0058f029` — an **obfuscated call site in `.text` that Ghidra does not disassemble**: the sequence at `0x0058f010` is `push eax; mov ecx,esi; push 0x0058f029; jmp PoseStore_ResolveHandle` (SecuROM-style push/jmp call). So the stub window runs SecuROM-obfuscated native code that lives in `.text`; the "region = .text" classification therefore does not mean ordinary game code. (`PgPrimitive_SubmitToGPU` also fired 13× inside the window during transitions, from `RenderFrameTimed`.)
- Protected-region callers exist outside the stub (gameplay logic): `.securom` call sites `024e084e` / `024ef30e` (`Pose_Copy`), `024ed8af` (`PoseStore_GetOwnerObject`), `02950021` (`ResolveHandle`), `0295b035` (`PgMaterial_ctor`), `03320064` (`GetPoseByHandle`); ~630k protected-caller hits of `GetPoseByHandle` in the run — protected routines routinely call plaintext helpers.
- Never fired: `ViewManager_Update` (unexpected — documented as the per-frame view update; verify), `RenderShell_InitDrawRecordTables`, `Technique_ResolveConstantRegisters`, `Shader_GetConstantRegisterIndex` (load-time).
- Render submit: `RenderShell_RenderFrameTimed` is normally called from the **registered task `RenderTask_RenderFrame` (`0x0046a290`, return address `0x0046a2a6`)** — dispatched by the task queue, not from a direct call; the device-lost path in `InGameShellState_Update` (`0x004c0b50`) is the only other caller. Occasionally (transitions) it fires inside the stub window.
- `RenderTask_RenderFrame` tail (2026-10-03, carrier `debug_vm_dump`): after the submit it calls thunk `0x0046ab80` (`jmp [0x024dc4ec]`, file target `.securom` `0x025628e0`) with `eax` = task node index (this task: 2; sibling task bodies `0x0046a270`/`0x0046a2c0`/`0x0046a300`/`0x0046a350` use 0/3/4/1) and `DAT_00e79dfc` on the stack. The loader patches the slot at startup to the plaintext, SecuROM-mutated `NodeArray_DecrementChildRefs` (`0x00518fa0`): over 0x30-byte nodes, `InterlockedDecrement` each child's refcount (+0x10; child list +0x14, count +0x28) and call `thunk_FUN_0256f820` (`0x0046aa60`, still VM-side) when it hits 0 — plausibly task-dependency signalling (inference). Not render work.
- **Proven VM -> plaintext callback (second run, stack chains)**: the `ResolveHandle` hits inside the stub window come from `FUN_0050c106` (a large SecuROM-mutated plaintext function) called by **protected code at `0x024f22b6` (Stext)**, which sits under `RunFrame -> FramePipeline (0x004c0e8b)`; the `0x0058f029` push/jmp site is reached from the same function (`0x0050c19c`). So the stub's native glue in Stext calls plaintext directly; chain: `0050c131 <- 024f22b6[PROT] <- 004c0e8b <- 004c0b1f <- 004c14f0 <- 00630ed1 (FrameTick) ...`. The chain scan is heuristic (stale stack data possible) but all three chains agree. `FUN_0050c106` (via thunks `FUN_00504a95`/`FUN_0050c0f0`) is the concrete plaintext entry the stub uses; its role is still unknown.


- `g_RenderShell` holds the **base** `LtiRenderer_vtbl` at frame time; the derived `RenderShell_vtbl` (0x00be84c0) overrides (incl. slot 15 `Flush`) never run. Type as `LtiRenderer`/`LtiRenderer_vtbl` (done in Ghidra).
- Calls per frame: Present/BeginScene/EndScene exactly 1:1 with `GameShell_FrameTick` count. `Reset` (slot 16) only on present-param change (resolution etc.), carries the new `D3DPRESENT_PARAMETERS`.
- Present args always all-NULL. Menu present params: 2560x1440, fullscreen, DISCARD, 60 Hz, interval DEFAULT, `hDeviceWindow=0x100b6`.
- The old "Begin/EndScene + Present live below this in SecuROM-encrypted thunks" note is FALSIFIED — they live in plaintext LtiRenderer_* functions; the producer/camera/draw-data chain (`SubmitWorldPackets` loop body, element staging, `RenderFrame` record walk) is also fully plaintext. What IS VM-protected (S0): the packet interpreter between the ring and the draw-records. GENERALIZED (2026-10-02): entering the SecuROM range is not proof of opacity — some entries are call gates with plaintext continuations (see `pandemic_engine.md` § SecuROM/VM boundary).

## D3D9 device vtable offsets

Header layout validated by the anchor: game applies render state via `+0xe4` = slot 57 = SetRenderState per d3d9.h (SetDialogBoxMode quirk at slot 20 included). Corrected labels: `+0x10c` = slot 67 SetTextureStageState (36-state per-stage loops, stride-`0x24` cache), `+0x114` = slot 69 SetSamplerState (4-arg `(0, MAGFILTER, LINEAR)`), SetTexture = slot 65. Pinned hook slots: Reset 16, Present 17, BeginScene 41, EndScene 42; swapchain GetSwapChain 14, GetPresentParameters 9.

## RenderFrame sweep (2026-10-02): RenderFrame internals — per-address facts in Ghidra

Navigate from the named symbols (all plate-commented). Only the non-obvious rules:
- Dx9 state wrapper: every D3D call in the render path goes through `g_LtiRenderer->dx9State` (`+0x5bc`; global `0x01175288` typed `LtiRenderer *`). Slot map = `Dx9StateWrapper_vtbl` struct members + `g_LtiRenderer` plate. Rules: the vtable is plain IDirect3DDevice9 order — wrapper slot n == device slot n for every slot (an earlier "omits one method / n+1" claim was wrong), and `dx9State` is the raw device object (the carrier's device VmtHook sees these calls); dirty-tracking caches are caller-side in the `Dx9_*` functions, updated after each device call — observing/forwarding at the device vtable is safe, ALTERING values there would desync `g_RenderStateCache` and the texture/sampler/RT caches.
- Precache: plates on `RenderShell_PrecacheLoadStep`/`RenderShell_PrecacheFinish`; `g_SuppressPresent` suppresses Present during precache frames.
- `Lti_LazyNameHash` (`0x008244a0`, 139 callers): per-site FNV-1a "Class::Method" IDs cached in `.bss`, read only by VM'd code — inert telemetry, NOT feature/device checks.
- `LtiRenderer_EndSubmit` StretchRects RT0 → backbuffer (`LtiRenderer+0x3ea4`) whenever they differ — existing RT→backbuffer seam for the S4 eye-image capture (also noted in `docs/stereo_design.md`).

## VR hook strategy (render side)

- Stereo submission point: `RenderQueue_SubmitWorldPackets` walks the ACTIVE views — an intrusive linked list (S0: head = `DAT_00d29e60`, an INDEX; link = `ViewEntry+0x4`; negative terminates) — and stages one 96-byte element per type-2/4 view: three `{byte-size, ptr}` pairs — {`0x30` camera staging `this+0xc2110+idx*0x30` (inline pos/serial/rot copy, site `0x0048ec3e`)}, {`0x810` live `ViewEntry*`}, {`0x680` frame-ctx block `this+0xd2950`}. Deref is at CONSUME time, post-walk, inside SecuROM-VM'd code (staging site `0x0048ef71`, count `[ESP+0x19a00]`, array `[ESP+0x79a0]`, cap 768). Producer-side per-eye duplication was ruled out — the consumer never reads view camera data; per-eye injection is at the GPU boundary (`docs/stereo_design.md`).
- Frame-level slots: `g_RenderShell` vtable slots 4/5 (`EndOfFrameHook`/`PostUpdateHook`, +0x10/+0x14) are `VirtHook_NoOp` on the live base vtable — claim via cloned-vtable swap on `g_RenderShell` (proven M3, 1:1 with frames).
- Camera data: per-view RenderShell sub-objects hang off `g_RenderShellPtr` at `idx*0x3a0`; they are copied once per FRAME into the frame-ctx 0x680 block (`this+0xd2950+0xEC`, S0) — not per view, not eye slots. The per-view camera handed to the consumer is `{0x810, ViewEntry*}` by POINTER (derefed post-walk) plus the inline `0x30` staging copy — patching these never moves the draw camera (`docs/stereo_design.md`).
- All draw state is cached by the Dx9 wrapper layer — caller-side caches in the `Dx9_*` functions (`g_RenderStateCache`, `0x105` entries, cleared by the invalidate slot; plus texture/sampler/RT caches). Device-vtable hooks that only observe/forward calls are safe (the caches are caller-side); altering values at the device level would desync them — alter at the `Dx9_*` function level instead.

## Open items

- **Wrapper vtable ADDRESS (type annotation DONE)**: `Dx9StateWrapper_vtbl` / `Dx9StateWrapper` structs exist in Ghidra and `g_LtiRenderer` is typed — all known slots decompile as named methods. The vtable is the D3D9 device's own (runtime-installed by `Direct3DCreate9`/`CreateDevice`, so no static address); remaining `slotNN` members can simply be named from d3d9.h order.
- `0x0117560c` — precache gate flag (if 0 while precache requested, frame bails early); semantics unexplored.
- `DAT_017d1818` / `DAT_017d2a94` — vtable'd singletons used by the type-8/9 passes (surface/rect providers); unidentified.
- The two frame-preamble hash keys (`0x5e84ea6d`, `0x16085a8d`, both with low dword `0xf011157a`) — presumed the two named world views; reverse the name strings if a registry-writer is found.

- Producer counter semantics in `g_RenderQueue` — **RESOLVED (S0)**: two packed 16-bit halves at +0x10 (countersA) and +0x14 (countersB). Producers (single-element and bulk paths in `SubmitWorldPackets`, plus the other nine producers) do: `countersB.low += count`, `countersA.high += count`, spin-wait until the consumer-derived position `(countersA.low + countersA.high) % capacity` matches the producer slot `(countersB.low + countersA.low) % capacity`. `countersA.low` is advanced only by the CONSUMER — which is SecuROM-VM'd (no plaintext writer), explaining M3's "queue+0x10 wraps like a ring position" and "+0x14 stayed 0" observations. Old packed-pair spin model: close, but the halves' roles were swapped.
- `g_RenderQueue2` (`0x00ff3650`) consumer — **narrowed (S0)**: its pointers sit inside the same 0x680 frame-ctx block handed per element; the VM interpreter at `0x0050f660` (call site `0x004c99f9`) is the prime suspect for consuming BOTH queues.
- View/portal table walk — **RESOLVED (S0)**: intrusive linked list; `DAT_00d29e60` = head INDEX (M3 "registered-view count" label wrong; also stored to frame-ctx `+0xd2a10`), link `ViewEntry+0x4`, negative terminates. Per-view element format, camera staging sites, and the 768-element staging cap are on the `SubmitWorldPackets` plate comment.
- `GameState3_Update` / `GameState2_Frontend_Update` internals — named by position, semantics unexplored.
- `0x0117527c` adapter remap table / multi-adapter handling in `RenderSystem_Init` — not explored (single-GPU assumption).
- `vt[4]`/`vt[5]`: ~~confirm anything actually CALLS them~~ **RESOLVED (M3)**: both slots called exactly once per frame by `GameShell_FrameTick` (claimable, mechanism proven via cloned vtable; survived alt-tabs/cutscene/mission load).
