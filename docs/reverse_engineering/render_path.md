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

**Diagram:** `../render_diagram.svg` — frame flow and the opaque VM stub (it also marks the mod's hook points; maintained with the implementation docs).

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
- The old "Begin/EndScene + Present live below this in SecuROM-encrypted thunks" note is FALSIFIED — they live in plaintext LtiRenderer_* functions; the producer/camera/draw-data chain (`SubmitWorldPackets` loop body, element staging, `RenderFrame` record walk) is also fully plaintext. What IS VM-protected (S0): the packet interpreter between the ring and the draw-records — but it ORCHESTRATES rather than computes: it calls plaintext for the draw-camera VP build (E1b: VM -> thunk `0x00506a26` -> `ViewContext_BuildCameraConstants 0x008591ac`, see the E1/E1b section below). GENERALIZED (2026-10-02): entering the SecuROM range is not proof of opacity — some entries are call gates with plaintext continuations (see `securom_vm.md`).

## D3D9 device vtable offsets

Header layout validated by the anchor: game applies render state via `+0xe4` = slot 57 = SetRenderState per d3d9.h (SetDialogBoxMode quirk at slot 20 included). Corrected labels: `+0x10c` = slot 67 SetTextureStageState (36-state per-stage loops, stride-`0x24` cache), `+0x114` = slot 69 SetSamplerState (4-arg `(0, MAGFILTER, LINEAR)`), SetTexture = slot 65. Pinned hook slots: Reset 16, Present 17, BeginScene 41, EndScene 42; swapchain GetSwapChain 14, GetPresentParameters 9.

## RenderFrame sweep (2026-10-02): RenderFrame internals — per-address facts in Ghidra

Navigate from the named symbols (all plate-commented). Only the non-obvious rules:
- Dx9 state wrapper: every D3D call in the render path goes through `g_LtiRenderer->dx9State` (`+0x5bc`; global `0x01175288` typed `LtiRenderer *`). Slot map = `Dx9StateWrapper_vtbl` struct members + `g_LtiRenderer` plate. Rules: the vtable is plain IDirect3DDevice9 order — wrapper slot n == device slot n for every slot (an earlier "omits one method / n+1" claim was wrong), and `dx9State` is the raw device object (the carrier's device VmtHook sees these calls); dirty-tracking caches are caller-side in the `Dx9_*` functions, updated after each device call — observing/forwarding at the device vtable is safe, ALTERING values there would desync `g_RenderStateCache` and the texture/sampler/RT caches.
- Precache: plates on `RenderShell_PrecacheLoadStep`/`RenderShell_PrecacheFinish`; `g_SuppressPresent` suppresses Present during precache frames.
- `Lti_LazyNameHash` (`0x008244a0`, 139 callers): per-site FNV-1a "Class::Method" IDs cached in `.bss`, read only by VM'd code — inert telemetry, NOT feature/device checks.
- `LtiRenderer_EndSubmit` StretchRects RT0 → backbuffer (`LtiRenderer+0x3ea4`) whenever they differ — existing RT→backbuffer seam for the S4 eye-image capture (also noted in `../plans/stereo_design.md`).

## View staging, frame-level slots and state caches

- View staging: `RenderQueue_SubmitWorldPackets` walks the ACTIVE views — an intrusive linked list (S0: head = `DAT_00d29e60`, an INDEX; link = `ViewEntry+0x4`; negative terminates) — and stages one 96-byte element per type-2/4 view: three `{byte-size, ptr}` pairs — {`0x30` camera staging `this+0xc2110+idx*0x30` (inline pos/serial/rot copy, site `0x0048ec3e`)}, {`0x810` live `ViewEntry*`}, {`0x680` frame-ctx block `this+0xd2950`}. Deref is at CONSUME time, post-walk, inside SecuROM-VM'd code (staging site `0x0048ef71`, count `[ESP+0x19a00]`, array `[ESP+0x79a0]`, cap 768). ~~The consumer never reads view camera data~~ (2026-10-06: re-interpreted — the walk-tail copy-back `0x0048F72D` proves the VM WRITES the staged block; whether it also READS it as draw-camera input is exactly what E2 tests — `view_and_camera.md` § camera-data accessors, `../plans/stereo_improvements.md` E2).
- Frame-level slots: `g_RenderShell` vtable slots 4/5 (`EndOfFrameHook`/`PostUpdateHook`, +0x10/+0x14) are `VirtHook_NoOp` on the live base vtable — claimable via cloned-vtable swap on `g_RenderShell` (proven 1:1 with frames; see `../plans/launcher.md`).
- Camera data: per-view RenderShell sub-objects hang off `g_RenderShellPtr` at `idx*0x3a0`; they are copied once per FRAME into the frame-ctx 0x680 block (`this+0xd2950+0xEC`, S0) — not per view, not eye slots. The per-view camera handed to the consumer is `{0x810, ViewEntry*}` by POINTER (derefed post-walk) plus the inline `0x30` staging copy — patching these never moved the draw camera in M3-era tests (done WITHOUT serial bumps and without the round-trip knowledge — re-interpreted 2026-10-06, and under re-test as E2 with the correct write+bump protocol; `view_and_camera.md`, `../plans/stereo_improvements.md` I2/E2).
- All draw state is cached by the Dx9 wrapper layer — caller-side caches in the `Dx9_*` functions (`g_RenderStateCache`, `0x105` entries, cleared by the invalidate slot; plus texture/sampler/RT caches). Device-vtable hooks that only observe/forward calls are safe (the caches are caller-side); altering values at the device level would desync them — alter at the `Dx9_*` function level instead.

## Draw-camera constant chain (E1/E1b, hardware-watch-proven 2026-10-06)

The `viewContextData`/VP production chain, mapped end-to-end — **all PLAINTEXT after the VM's
orchestration thunk** (plates on the named symbols in Ghidra; interpretation in
`view_and_camera.md` § Where the draw camera lives; plan context in `../plans/stereo_improvements.md`):

```
camera entity pose (quat + pos, heap record; values originate in game logic/VM)
  -> CameraTable_FillFromPose (0x0070ae50, plaintext; called from CameraTable_FillLoop
     FUN_0070f430's 5-slot x 0x620 loop):
       CameraEntity_GetPoseRecord (0x0042ee50) -> D3DXQuaternionNormalize
       -> D3DXMatrixRotationQuaternion -> position + w=1.0
       -> Matrix_Copy3x4 -> g_CameraTable entry (0x014A2EE0, STATIC, ~1.7 fills/frame)
       [g_CameraTable = the SINGLE injection point: also read by the culling/fov
        consumers (0x0048067E family, global frame-ctx 0x017cf980). IMPLEMENTED +
        live-verified (2026-10-07): carrier MidHook at 0x0070AEF8 rewriting the
        just-filled entry with the HMD-union pose — see the conventions subsection
        below for WHY the composition is non-obvious]
  -> per builder call: CamPose_ClearEntryPose (0x004665b0) clears the stack entry's pose
     from g_CameraPoseClearBlock (0x00DFBBD0, static zero template), then a Matrix_Copy3x4
     copies the live pose from g_CameraTable -> stack entry, then CamPose_FillEntryFov
     (0x00466615) fills fov from static 0x00B9B688
  -> VM'd packet interpreter (orchestrates only)
  -> VMThunk_ViewContext_BuildCameraConstants (0x00506a26, no static xref = VM-called)
  -> ViewContext_BuildCameraConstants (0x008591ac):
       camera object = *(ctx+0x28)          (null -> identity defaults)
       near/far/fov via self-indexed obj[obj[0]*0x1c + 0x14/0x15/0x16/0x1d]
       entry rot+pos copied VERBATIM into ctx+0xaa0 (Matrix_Copy3x4 @0x008592a1)
       -> INVERTED into the view slot (FUN_008225c0 = Matrix_Inverse4x4,
          call @0x008593db, result copied back @0x008593e4)
       projection built in plaintext (LH: clip.w z-coeff +1.0 @ctx+0xb4c; m00 =
         -1/xscale from the fov tan table DAT_00cf1900, aspect from g_RenderShell
         fields 0xae6/0xae7/0xaf2/0xaf3/0xaf5 — always the SCREEN aspect's)
       D3DXMatrixMultiply(view@ctx+0xaa0, proj@ctx+0xb20) = VP matrix
       Matrix_Copy3x4(VP -> scratch 0x017D04E0)            [call site 0x00859562]
  -> record fill (once per frame, frame-build time, ~1.0-1.7 events/frame):
       scratch -> g_ViewContextTable record
       0x0046718c = Matrix_Copy3x4 (idx-6 record); 0x004673bf = inline fstp (idx-12)
       record addr = 0x018c45e0 + (bufIdx*32 + viewIdx)*0x70, bufIdx = [0x00ff364c]
       BOTH double-buffer bases written per event; NOT per submit
  -> upload gate 0x00855a78 ([esp+0x18] = this pass's record ptr)
  -> Dx9_SetVertexShaderConstantF -> device slot 94
```

### Matrix & handedness conventions (probe-proven 2026-10-07)

These could NOT be settled by static analysis (sign-blind probe evidence, runtime-signed
projection coefficients) — a runtime transfer-function probe (carrier
`debug_camtable_probe`) measured them; six live rounds of "sign convention" fixes were all
consistent with the facts below being unknown. Details + the full derivation:
`../plans/stereo_improvements.md` § The entry convention; plates on
CameraTable_FillFromPose / ViewContext_BuildCameraConstants.

- **LEFT-HANDED pipeline** (classic D3D LH): the projection's clip.w z-coefficient is
  **+1.0** (ctx+0xb4c store in the builder) ⇒ `clip.w = +z_view`, view z is positive IN
  FRONT ⇒ the camera local z axis is FORWARD (not backward). `m00 = -1/xscale` with the
  tan-table sign runtime-dependent. The game's projection is always the screen aspect's.
- **Matrix majority / entry semantics** (the non-obvious one): the camera entries
  (g_CameraTable slots, builder stack entries) are row-major 3x4s (rotation + pos + w=1),
  but they are NOT view matrices — the builder copies the entry verbatim and INVERTS it
  into the view. The RENDERED camera's axes are the entry's **ROWS**: R = −row0, U = row1,
  F = row2. Transfer law for any rewrite: writing `E' = E·M` renders `axes' = M⁻¹·axes`
  (world-side application of the INVERSE). To apply a desired aim-following LOCAL rotation
  L, write the closed form `E' = S_r·L⁻¹·S_r·E` (S_r = diag(measured row signs) =
  diag(−1,1,1) in this build; composite quat `(qx,−qy,+qz,qw)` for `L = (−qx,−qy,+qz,qw)`).
- **Consequence for consumers of decomposed VP rows** (e.g. the carrier's `vp_camera::decompose`,
  identity-verified on real rows): its basis is consistent with the rows above only up to the
  projection-coefficient signs — near-identity cameras make every wrong row/column/handness
  reading locally consistent. Validate sign-sensitive consumers against a YAWED camera or
  the probe, not an aligned one.

- **Record census** (carrier per-record upload tally, `view: rec ...` log lines): world-scale records
  **idx 6 and idx 12** — same VP row0 = same camera, 60–87k main-RT uploads/10s each; shadow-atlas
  (1024x4096) idx 7/8; idx 5 exactly-once-per-submit main+offscreen pair; idx 4/13/11 minor;
  idx 2/3/9/10 transient (menu/load). Stable across sessions.
- **Cadences**: scratch ~4.4 writes/frame (builder runs per active view) vs record fill ~1.0–1.7/frame
  — the builder runs more often than records refresh (consumption is change-gated).
- **Methodology lesson**: the fill code lives in SecuROM-mutated `.text` (~0x004671xx–0x004674xx) with
  NO defined function and no static callers — invisible to decompiler-text static hunts (why the
  2026-10-03 static writer hunt was negative). `debug_watch=addr:` on the target address is the probe
  that finds writers in such regions; watch the EIP + surrounding bytes, then decode by hand.
- **Consequences**: record-level per-eye rewrites at pass boundaries are interference-free (the fill
  is once per frame, before the passes; the S2c replay pass re-reads unchanged records — consistent
  with its proven state-safety). Camera VALUES enter at the camera object (`ctx+0x28`) — its writer is
  unknown and is the E2/I2 injection question.

## Open items

- **Wrapper vtable ADDRESS (type annotation DONE)**: `Dx9StateWrapper_vtbl` / `Dx9StateWrapper` structs exist in Ghidra and `g_LtiRenderer` is typed — all known slots decompile as named methods. The vtable is the D3D9 device's own (runtime-installed by `Direct3DCreate9`/`CreateDevice`, so no static address); remaining `slotNN` members can simply be named from d3d9.h order.
- `0x0117560c` — precache gate flag (if 0 while precache requested, frame bails early); semantics unexplored.
- `DAT_017d1818` / `DAT_017d2a94` — vtable'd singletons used by the type-8/9 passes (surface/rect providers); unidentified.
- The two frame-preamble hash keys (`0x5e84ea6d`, `0x16085a8d`, both with low dword `0xf011157a`) — presumed the two named world views; reverse the name strings if a registry-writer is found.

- Queue counter semantics — **RUNTIME-RESOLVED (S4-5 HUD measurement, 2026-10-06, live-verified menu +
  gameplay via `hud2 raw` bursts)**: `+0x10` high16 = pending-unconsumed element count (producers `+=`
  during the frame — visible nonzero at SubmitToGPU entry; the VM consumer CLEARS it and advances low16
  between SubmitToGPU entry and BeginSubmit's Present); `+0x10` low16 = ring position (moves on
  publish/consume; FROZEN during both pass walks and the inter-pass gap — `p1=b2=p2=b0` in every sampled
  frame); `+0x14` NEVER moves at runtime (unused by the live path — the S0 static model's
  `countersB.low += count` producer step does not manifest). The static packed-halves model was close on
  the halves' roles but wrong on their meaning and the consume timing. CONSUMPTION POINT: both queues are
  fully consumed BEFORE pass 1 begins drawing (inside pass-1 SubmitToGPU, pre-BeginSubmit) — this also
  refines the frame chain: the VM interpreter's consume effect lands inside the pass-1 submit window, and
  HUD/2D records therefore exist in the record table both passes walk (S4-5 HUD conclusion: one-eye HUD
  impossible; see docs/plans/stereo_design.md §S4-5).
- `g_RenderQueue2` (`0x00ff3650`) consumer — **narrowed (S0)**: its pointers sit inside the same 0x680 frame-ctx block handed per element; the VM interpreter at `0x0050f660` (call site `0x004c99f9`) is the prime suspect for consuming BOTH queues.
- View/portal table walk — **RESOLVED (S0)**: intrusive linked list; `DAT_00d29e60` = head INDEX (M3 "registered-view count" label wrong; also stored to frame-ctx `+0xd2a10`), link `ViewEntry+0x4`, negative terminates. Per-view element format, camera staging sites, and the 768-element staging cap are on the `SubmitWorldPackets` plate comment.
- **Camera-object pose writer — RESOLVED (E2b complete, 2026-10-06)**: the draw-camera chain
  (see the E1/E1b section above) is mapped end-to-end. Camera VALUES originate in the camera
  entity's quat+pos (heap record, game logic / VM-side), flow through PLAINTEXT
  `CameraTable_FillFromPose` (0x0070ae50, D3DX quat→rotation) into the STATIC `g_CameraTable`
  (0x014A2EE0), which feeds the VP builder AND the culling/fov consumers. The single injection
  point is g_CameraTable post-fill — IMPLEMENTED (2026-10-07): carrier MidHook at 0x0070AEF8,
  the instruction after the fill's Matrix_Copy3x4 call; the E2-disproven
  ViewEntry entry-injection protocol is retired. Verdicts + implementation notes:
  `../plans/stereo_improvements.md`.
- `GameState3_Update` / `GameState2_Frontend_Update` internals — named by position, semantics unexplored.
- `0x0117527c` adapter remap table / multi-adapter handling in `RenderSystem_Init` — not explored (single-GPU assumption).
- `vt[4]`/`vt[5]`: ~~confirm anything actually CALLS them~~ **RESOLVED (M3)**: both slots called exactly once per frame by `GameShell_FrameTick` (claimable, mechanism proven via cloned vtable; survived alt-tabs/cutscene/mission load).
