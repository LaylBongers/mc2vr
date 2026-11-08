# Hooks

Mod-side hooking rules + inventory. SecuROM background: [securom_vm.md](reverse_engineering/securom_vm.md).

## Rules (proven, do not re-litigate)

- SecuROM enforcement is inert under everything we do (live `.text` inline patches, in-process `.data` writes, direct VM-stub thunk calls, DLL injection, cross-process reads; multi-minute runs, zero reaction). Still: never attach a debugger/ptrace; the carrier is the only sanctioned probe.
- Plaintext `.text` only. Never hook anything at `0x01a48000+` or VM-stub thunks (interpreter stub `0x0050f660`, pose-getter thunk `0x0048bf00` — use plaintext call sites). CALLING stubs is fine (e.g. `GetD3DDevice`, proven).
- The device vtable is the one sanctioned vtable patch (via clone). Device-vtable hooks that only observe/forward are safe (the game's state caches are caller-side in the `Dx9_*` functions); ALTERING values at the device level would desync them (`g_RenderStateCache`, texture/sampler/RT caches) — alter at the `Dx9_*` function level instead, or rewrite scratch copies as `view_rewrite.cpp` does.
- Hook handlers must match the original convention; zero-arg void functions are convention-agnostic on i386.
- Function replacement is sanctioned: a plaintext engine function may be reimplemented in the carrier (InlineHook to our implementation, calling back into game functions), within reason and gradually — RE the callees' conventions first, and prefer replacing the smallest function that owns the decision. Plan using it: [render_resolution.md](render_resolution.md).
- Hook objects are leaked by design: destructors would restore vptrs/bytes during process teardown after the target objects may be freed.

## SafetyHook facts

- Install needs NO thread suspension: SafetyHook v0.7.0 inline install is trap-based (page guard + VEH IP fixup) — atomic w.r.t. execution. External suspension is FORBIDDEN (v0.7.0 heap-allocates during `create_inline`; a suspended thread holding the CRT heap lock would deadlock).
- VmtHook (cloned-vtable vptr swap) works on DXVK's MinGW-built objects (`VMT_HEADER=2` matches DXVK's Itanium vtables); safe from a foreign thread (aligned pointer store; in-flight calls keep the old valid vtable); survives device-lost + `Reset` cycles.
- MidHook = register-context probe at arbitrary instructions — the tool for thiscall/unknown-convention sites (read ECX/ESP from context, no dispatch semantics).

## Inventory

| Site | VA | Type | Purpose |
|---|---|---|---|
| `GameShell_FrameTick` | `0x00630e10` | InlineHook | frame counter, 10 s timing reports, `hooks::frame_count()` |
| `GetD3DDevice` thunk | `0x0047f2f0` | direct call (never hook — hot path) | only the `debug` pre-probe calls it (first Present); the device itself is captured at `CreateDevice`. Slot patched to `GetD3DDevice_Impl` `0x00403160` at runtime |
| device vtable | runtime | VmtHook | Present 17 / BeginScene 41 / EndScene 42 / Reset 16; SetVertexShaderConstantF 94 = the camera channel ([view_rewrite.md](view_rewrite.md)); SetRenderTarget 37 = pass gate; StretchRect 34 + UpdateSurface 30 = eye-RT redirect ([stereo.md](stereo.md)); UpdateTexture 31 diagnostic; GetBackBuffer 18 identity; SetRenderState@57 was the slot-pinning anchor |
| `Direct3DCreate9` IAT thunk + `IDirect3D9` vtable | thunk `0x00a4e892` (jmp `[0x00b05620]`); CreateDevice slot 16 | InlineHook (attach, always) + VmtHook | device VmtHook installed inside CreateDevice ([carrier.md](carrier.md)); [pacing.md](pacing.md) vsync unlock; the engine's own CreateDevice call site is VM-gated (`FUN_0074c9b0` → ptr `0x024cd09c`, never hooked) |
| `LtiRenderer_BeginSubmit` entry | `0x0074aaa0` | MidHook | one-shot driver probe; dormant |
| `RenderCmd_ExecuteStream` | `0x008569d0`, MidHook at opcode cmp `0x008569f5` (EAX=opcode, 27 ops) | MidHook | opcode histogram + [stereo.md](stereo.md) stream tap |
| `RenderQueue_SubmitWorldPackets` view loop | `0x0048e620`, MidHook at `0x0048e9ea` (ESI=idx, ECX=type; entry+0x7e4 per-view object, 3rd table `0x014095e0` stride 0x20) | MidHook | view aggregation telemetry |
| `g_RenderShell` slots 4/5 call sites in `GameShell_FrameTick` | `0x00630ee7` (slot 5 PostUpdateHook) / `0x00630f99` (slot 4 EndOfFrameHook) — the `mov ecx,[0x00dfb2f8]` starting each virtual call; both slots are the shared no-op `VirtHook_NoOp` `0x006b9360` | MidHook ×2 (attach) | per-frame orchestration: [ipc.md](ipc.md) event drain, pose sample point; proven 1:1 with frames. Replaced the cloned-vtable claim 2026-11-08 (no vptr dependency, so the `0x00a7d950` reinit-reinstalls-vtable risk is gone) |
| `g_RenderQueue` counters | `0x00ff3618` | poller thread (250 ms), 10 s reports | producer rhythm telemetry |
| viewContextData upload gate | `0x00855a78` (`cmp [edi+0xd8],0`, EDI = current technique) | MidHook | publishes the exact viewContextData/ViewProj register map ([view_rewrite.md](view_rewrite.md)) |
| rt_probe (`debug_rt_probe=on` only) | MidHooks `0x007492f0` OnScreenResize entry, `0x007494e0` SetupRenderTargets entry, `0x0075078d` FUN_0074fae0 front-ref read; device slots 14/18/23/28/29/36/38/40 | MidHook ×3 + VmtHook | log-only render-target/size census for [render_resolution.md](render_resolution.md) |
| camera-table fill | `0x0070AEF8` | MidHook | union HMD-pose injection ([camera.md](camera.md)) |
| view-context builder tan site + camera clearance | `0x0085943B` / `0x007107F9` | MidHook ×2 | culling alignment ([culling.md](culling.md)) |

## Frame-level strategy

- Reuse the game's own timing (`g_FrameDeltaSec`, `g_Dt`); VR pacing bypasses/neutralizes the adaptive framerate path (`g_FrameratePolicy` / `AdaptiveFramerate_Govern`) — so far unneeded, see [pacing.md](pacing.md).
- Never hook via the SecuROM wrapper pointers (runtime-only) or inside `0x01a48000+`.
- The ring/packet interpreter is VM-protected — probe it by bracketing plaintext call sites, never by hooking the stub.
