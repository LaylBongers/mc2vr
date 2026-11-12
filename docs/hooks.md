# Hooks

Mod-side hooking rules + inventory. SecuROM background: [securom_vm.md](reverse_engineering/securom_vm.md).

## Rules (proven, do not re-litigate)

- SecuROM enforcement is inert under everything we do (live `.text` inline patches, in-process `.data` writes, direct VM-stub thunk calls, DLL injection, cross-process reads; multi-minute runs, zero reaction). Still: never attach a debugger/ptrace; the carrier is the only sanctioned probe.
- Plaintext `.text` only. Never hook anything at `0x01a48000+` or SecuROM thunks themselves (e.g. thunk `0x0050c660`, pose getter `0x0048bf00` — use plaintext call sites). CALLING stubs is fine (e.g. `GetD3DDevice`, proven).
- The device vtable is the one sanctioned vtable patch (via clone). Device-vtable hooks that only observe/forward are safe (the game's state caches are caller-side in the `Dx9_*` functions); ALTERING values at the device level would desync them (`g_RenderStateCache`, texture/sampler/RT caches) — alter at the `Dx9_*` function level instead, or rewrite scratch copies as `view_rewrite.cpp` does. The exception is scene MSAA's surface mapping ([msaa.md](msaa.md)): it substitutes surfaces the same way on every bind, so the caches (which hold the engine's own pointers) stay consistent. The occlusion-query pairs substitute at the object instead: the engine's ring holds our pair objects, which own its queries ([occluder_boxes.md](occluder_boxes.md)).
- Hook handlers must match the original convention; zero-arg void functions are convention-agnostic on i386.
- Function replacement is sanctioned: a plaintext engine function may be reimplemented in the carrier (InlineHook to our implementation, calling back into game functions), within reason and gradually — RE the callees' conventions first, and prefer replacing the smallest function that owns the decision. Example: the render-size policy at `OnScreenResize`'s boundary ([render_resolution.md](render_resolution.md)) — which also shows the limit: a callee with opaque (VM) register inputs rules out reimplementation, so the inputs are set instead.
- Hook objects are leaked by design: destructors would restore vptrs/bytes during process teardown after the target objects may be freed.

## SafetyHook facts

- Install needs NO thread suspension: SafetyHook v0.7.0 inline install is trap-based (page guard + VEH IP fixup) — atomic w.r.t. execution. External suspension is FORBIDDEN (v0.7.0 heap-allocates during `create_inline`; a suspended thread holding the CRT heap lock would deadlock).
- VmtHook (cloned-vtable vptr swap) works on DXVK's MinGW-built objects (`VMT_HEADER=2` matches DXVK's Itanium vtables); safe from a foreign thread (aligned pointer store; in-flight calls keep the old valid vtable); survives device-lost + `Reset` cycles.
- MidHook = register-context probe at arbitrary instructions — the tool for thiscall/unknown-convention sites (read ECX/ESP from context, no dispatch semantics).

## Inventory

Code: one file per group in `src/carrier/hooks/`, installed in the order of `hooks/install.cpp`. Debug-only sites live in their tool under `src/carrier/debug/`. Every hook is created through `hooks/site.hpp`, which refuses sites inside an original replaced by a reimplementation (`hooks/reimpl.cpp`, [reimpl.md](reimpl.md)). Layout rules: [code_structure.md](code_structure.md).

| Site | VA | Type | Purpose |
|---|---|---|---|
| `GameShell_FrameTick` | `0x00630e10` | InlineHook | frame counter, 10 s timing reports, `hooks::frame_count()`; lockstep gate + display-delta `g_FrameDeltaSec` overwrite before the original runs (`pacing.cpp`, [pacing.md](pacing.md)) |
| `GetD3DDevice` thunk | `0x0047f2f0` | direct call (never hook — hot path) | not called by the carrier (the device is captured at `CreateDevice`); calling it is safe once the loader has patched the slot. Slot patched to `GetD3DDevice_Impl` `0x00403160` at runtime |
| device vtable | runtime | VmtHook | Present 17 / BeginScene 41 / EndScene 42 / Reset 16; TestCooperativeLevel 3 (forwarded; answers `RenderSystem_CheckDevice`'s call once with DEVICENOTRESET to drive a render-size switch, [render_resolution.md](render_resolution.md)); SetVertexShaderConstantF 94 = the camera channel ([view_rewrite.md](view_rewrite.md)); SetRenderTarget 37 = pass gate + scene MSAA's target mapping; SetDepthStencilSurface 39 / SetTexture 65 = scene MSAA's depth mapping and resolve-before-sampling ([msaa.md](msaa.md)); StretchRect 34 = observed + the original-method trampoline for carrier blits; SetRenderState@57 was the slot-pinning anchor |
| `Direct3DCreate9` IAT thunk + `IDirect3D9` vtable | thunk `0x00a4e892` (jmp `[0x00b05620]`); CreateDevice slot 16 | InlineHook (attach, always) + VmtHook | device VmtHook installed inside CreateDevice ([carrier.md](carrier.md)); [pacing.md](pacing.md) vsync unlock; the engine's own CreateDevice call site is VM-gated (`FUN_0074c9b0` → ptr `0x024cd09c`, never hooked) |
| `RenderCmd_ExecuteStream` | `0x008569d0` | InlineHook (replacement) | the reimplementation ([reimpl.md](reimpl.md)); its per-command debug tap feeds the opcode histogram and the stream capture |
| `PgPrimitive_SubmitToGPU` | `0x00855690` | InlineHook (replacement) | the reimplementation: the stereo frame, LEFT then RIGHT pass ([stereo.md](stereo.md)), uploading the per-eye view record (`view_rewrite::eye_view_record`) |
| `ViewContext_BuildCameraConstants` | `0x008591a0` | InlineHook (replacement) | the reimplementation; the HMD cull widening as a direct call ([culling.md](culling.md), [reimpl.md](reimpl.md)) |
| `RenderShell_SetupViewports` | `0x0085a9b0` | InlineHook (replacement) | the reimplementation; the HUD layer's UI scale / aspect mode policy as a direct call ([hud.md](hud.md), [reimpl.md](reimpl.md)) |
| `PgLtiBloom_EmitPasses` | `0x0074fae0` | InlineHook (replacement) | the reimplementation; the composite without motion blur and depth of field ([motion_blur.md](motion_blur.md)) |
| `LtiRenderer_BeginSubmit` | `0x0074aaa0` | InlineHook (replacement) | the reimplementation, with the replay-pass GPU drain skip as a direct call ([pacing.md](pacing.md)) |
| `RenderQueue_SubmitWorldPackets` view loop | `0x0048e620`, MidHook at `0x0048e9ea` (ESI=idx, ECX=type; entry+0x7e4 per-view object, 3rd table `0x014095e0` stride 0x20) | MidHook | view aggregation telemetry |
| `g_RenderShell` slots 4/5 call sites in `GameShell_FrameTick` | `0x00630ee7` (slot 5 PostUpdateHook) / `0x00630f99` (slot 4 EndOfFrameHook) — the `mov ecx,[0x00dfb2f8]` starting each virtual call; both slots are the shared no-op `VirtHook_NoOp` `0x006b9360` | MidHook ×2 (attach) | OpenXR view-size hand-off, [xr_session.md](xr_session.md) (slot 5); proven 1:1 with frames. Hooking the call sites rather than the vtable avoids depending on the vptr, which `0x00a7d950` re-installs on reinit |
| `g_RenderQueue` counters | `0x00ff3618` | poller thread (250 ms), 10 s reports | producer rhythm telemetry |
| `PgLtiRendererPc_SetupRenderTargets` wrap | `0x007494e0` (thiscall, 1 stack arg, callee-cleaned; wrapped as fastcall so EDX passes through) | InlineHook (attach) | [render_outputs](render_outputs.md) takeover at setup exit; render_size aspect re-apply |
| `RenderSystem_CheckDevice` restore tail | `0x0074d087` (`mov edx,[0x01175288]` after `RenderSystem_RestoreDeviceResources` / `Dx9State_ApplyDefaultRenderStates`) | MidHook (attach) | render_outputs retake after Reset (setup is not re-run on Reset) |
| render_size (`render_size=WxH`) | `0x007492f0` OnScreenResize entry (W `[esp+4]`, H EAX), `0x007494d5` its epilogue, `0x00749500` SetupRenderTargets cfg → fields store (ECX/EAX) | MidHook ×3 (attach) | render size policy: inputs rewritten, aspect/mouse post-fixed ([render_resolution.md](render_resolution.md) §1) |
| bloom adaptive-surface slots (`hooks/adaptation.cpp`) | `0x0074e83d` (ESI = &_AdaptiveLuminanceSurface) / `0x0074e9c3` (EDI = &_pPrevAdaptiveLuminanceSurface), 5-byte log pushes in `PgLtiBloom_CreateSurfaces` | MidHook ×2 (attach) | record where the auto-exposure state lives; saved before / restored after a render-size switch |
| `PgPrimitive_PublishFrame` return | `0x0074add6` | MidHook (attach) | publishes the HUD/marker record ranges and the world-marker quads with the record list ([hud.md](hud.md)); taps `debug_build_census` |
| `Hud_DrawQuad` before its renderer call | `0x00617d91` (6-byte `mov ecx,[0x00dfb2f8]`; XMM5/XMM6 = centre, `[esp+0x10]` = vertices, `[ebp+8]` = depth) | MidHook (attach) | each world-marker quad for the per-eye placement ([hud.md](hud.md)) |
| `Widget_CorrectForResolution` after the view W/H loads | `0x00627d19` (XMM0 = W, XMM5 = H; flags live) | MidHook (attach) | the HUD layer's 16:9 layout ([hud.md](hud.md)); the re-layout trigger runs from the PostUpdateHook site |
| `GuiMgr_OnScreenResize` W / H loads | `0x00609f1d` (ECX = W) / `0x00609f2d` (EDI = H; flags live) | MidHook ×2 (attach) | GUI screen info + scripts' resize event at the HUD layer's 16:9 size ([hud.md](hud.md)) |
| world / pinned markers UI-scale loads | `0x0060f659` (ECX = view*0xe80) / `0x00611809` (EDI = view*0xe80); XMM0 | MidHook ×2 (attach) | the stock eye-image scale back for the two eye-image GUI readers ([hud.md](hud.md)) |
| HUD step-4 W/H loads | `0x0061007d` (EAX/ECX), `0x00612b91` (XMM5/XMM2), `0x00612c7a` (EDI/EAX; x87 live) | MidHook ×3 (attach) | off-screen indicators on the HUD layer's screen ([hud.md](hud.md)) |
| `GuiRenderer_BeginDisplay` view-rect copy | `0x004af05a` (EDI = renderer, rect at +0x6c048) | MidHook (attach) | Scaleform's display rect = the HUD layer's screen ([hud.md](hud.md)) |
| camera-table fill | `0x0070AEF8` | MidHook | union HMD-pose injection ([camera.md](camera.md)) |
| `CamHuman_Update` final matrix | `0x0071e611` (`mov ecx,[esp+0xd8]`, 7 bytes; EBX = camera, matrix at ESP+0xa0) | MidHook (only with `first_person=on`) | first-person eye position + obstruction off ([first_person.md](first_person.md)); becomes a direct call when `CamHuman_Update` is reimplemented |
| camera clearance | `0x007107F9` | MidHook | culling alignment ([culling.md](culling.md)) |
| occlusion-query ring: `RenderShell_InitDrawRecordTables` pointer set / `LtiRenderer_ReleaseRenderTargets` entry / emitter id mask | `0x0085405F` / `0x0074A850` / `0x0046FA0D` | MidHook ×2 + byte patch (attach) | `g_OcclusionQueryArray` → the carrier's 4096-slot per-eye ring, released with the engine's; ids widened to 12 bits ([occluder_boxes.md](occluder_boxes.md)); `debug_occlusion_off` NOPs the box calls `0x00468eed` / `0x00468f12` instead |

## Frame-level strategy

- Reuse the game's own timing (`g_FrameDeltaSec`, `g_Dt`); VR pacing bypasses/neutralizes the adaptive framerate path (`g_FrameratePolicy` / `AdaptiveFramerate_Govern`) — so far unneeded, see [pacing.md](pacing.md).
- Never hook via the SecuROM wrapper pointers (runtime-only) or inside `0x01a48000+`.
- Protected routines behind thunks (e.g. `0x0050c660` → `0x024F2170`, a handle-event loop, [reverse_engineering/securom_vm.md](reverse_engineering/securom_vm.md)) — probe them by bracketing their plaintext call site (here `0x004c99f9`), never by hooking inside.
