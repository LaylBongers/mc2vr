# Render resolution decoupling (window → HMD native)

Planned; revised 2026-11-07 (supersedes the 2026-10-11 field-seam-only plan), not implemented. Goal: per-eye passes render at the HMD's native resolution (2016×2240/eye, [openxr_proton.md](openxr_proton.md)) while the window, present params and swapchain stay stock (fullscreen 2560×1440).

RE basis: [reverse_engineering/rt_dims_source.md](reverse_engineering/rt_dims_source.md) (size authorities, readers, aspect snapping) and [reverse_engineering/render_target_table.md](reverse_engineering/render_target_table.md) (output surfaces, lifecycle).

## Principles

- **Change render size only, not display size.** The engine already keeps two size authorities: display = `g_RenderCfg` (window, present params, mode lookup, mouse) and render = RenderShell `+0x2b98/+0x2b9c`. Stock sets them equal. We change render only, so the swapchain, window, `Reset` behavior and `RenderSystem_ApplySettings` stay stock.
- **Own engine objects; don't intercept the device.** The carrier creates the output surfaces and puts them into the engine's render target table; size policy lives in a carrier-owned `OnScreenResize`. Device vtable hooks stay observe/forward plus Present composition. Device-level substitution (fake getters, per-call redirects) is a backstop only.
- **Replace renderer functions gradually where a seam is too thin** ([hooks.md](hooks.md)): start with the smallest plaintext function that owns the decision, reimplement it calling back into game code, and grow only where a seam proves insufficient. Candidates here, in order: `LtiRenderer_OnScreenResize`, the SetupRenderTargets plaintext tail, the Present path (`LtiRenderer_Dx9_Present`), and if needed the composite/EndSubmit stage.

## 1. Render size: carrier policy at `OnScreenResize`'s boundary

- **Not a reimplementation** (RE 2026-11-08, [reverse_engineering/rt_dims_source.md](reverse_engineering/rt_dims_source.md)). `OnScreenResize` calls a SecuROM VM thunk (`FUN_00854da0`, ×2) whose inputs are the live register state. So the engine's own function runs, with the carrier's render size as its input:
  - MidHook at entry (`0x007492f0`): rewrite W (`[esp+4]`) and H (EAX) to the render size. The engine then writes the fields, float copies, viewports (`RenderShell_SetupViewports`), UI half-dims and the Bink-rect rescale (its new/old ratios come from the same inputs) consistently.
  - MidHook at the epilogue (`0x007494d5`, single branch target): post-fix aspect per policy (§5) instead of 4:3/16:9 snapping, and scale `g_MouseScaleX/Y` by render ÷ cfg, so the menu mouse maps into render space. The callees ignore aspect in single view, so the post-fix is equivalent to a replacement.
- Every size path funnels through it: device create, device restore and menu resolution change all call `OnScreenResize(g_RenderCfg)`.
- SetupRenderTargets copies cfg → fields itself: MidHook at `0x00749500` rewrites ECX/EAX (cfg W/H) before the two stores. Aspect is re-applied after Setup returns (the existing wrap), because the mutated stretch re-snaps it.
- rt_probe's own `OnScreenResize` entry MidHook moves into this module as a callback (same address).
- Per-frame invariant check (existing RenderShell slot hook):
  - Field drift → log only; surfaces are already sized, so this is a canary for unknown writers, including VM code.
  - Aspect/mouse drift → re-apply (script/cvar writers `FUN_005b0100`, `FUN_005b2c70`).
- Size source: needed before `RenderSystem_CreateDevice` (it runs inside the first `SetupRenderTargets`, ~instantly after resume) → conf value; host-published OpenXR recommended size over [IPC](ipc.md) if it's up at boot. A mid-run change goes through the engine's own reset path. `render_scale` conf for perf.

## 2. Output surfaces: carrier-owned table entries

- After SetupRenderTargets, create native-size output surfaces and write them into the table:
  - front image (RT usage, swapchain format) → `+0x3ea4` and the front ref `+0x3e98`;
  - native depth → `+0x3e94`, and `+0x3ea0` when it aliases the auto depth.
- Each slot holds its own reference (the engine's release paths Release them); the carrier drops its own in the Reset hook (D3DPOOL_DEFAULT before Reset).
- From then on the engine never references the real swapchain backbuffer or auto depth. Everything that draws into "the backbuffer" (gameplay composite, HUD, menus, movies, loading) lands natively in our image.
- **Replace the front and the auto depth together.** Op 6 bakes both (`+0x3e98` / `+0x3e94`), and they must match in size and multisample. Today both are 8× MSAA (the swapchain's): either keep the eye image MSAA and resolve on capture, or make both ms=0 (HUD loses MSAA).
- **Re-register replacements** with the PrecacheSurface registry under the engine's names (its `vfn[+0x1c]` updates entries whose pointer differs).
- **Per-eye output.** Live verdict ([reverse_engineering/render_target_table.md](reverse_engineering/render_target_table.md)): the gameplay composite binds through render-command op 6 with pointers baked before pass 1. So a plain per-pass table swap only reaches the EndSubmit path (menus/loading). Options:
  - (a) **Translate at execution:** in `RenderCmd_ExecuteStream` op 6 (`0x00856a95`; we already MidHook its opcode dispatch at `0x008569f5`), map the baked front/auto depth → this pass's eye image/depth. Combined with the per-pass table swap for EndSubmit, the engine renders each eye straight into its own image. Zero-copy, and with the ring extension the eye images ARE the shared ring slots ([shared_textures.md](shared_textures.md)). This is an engine-level seam (the command record format is known) — a candidate for replacing the op-6 handler outright.
  - (b) **One front, captured per pass:** copy front → ring slot at each pass end. Always valid, one same-size copy per eye.
  - Prefer (b) first (track 3 at window size), then (a).
- **Retires:**
  - the monitor pin;
  - backbuffer-sourced capture;
  - the DISCARD stale-page hazard (the engine no longer writes the real backbuffer);
  - likely the pass-2 main-RT → eye-RT redirect (`eye_replay.cpp`) — confirm nothing reads pass-1 main RT after the boundary;
  - the size-keyed `view_rewrite::pass_is_main` gate, which re-keys on `g_RT_MainColorSurf` identity read from the table.
- Carrier eye/ring surfaces size from our images, not from `g_main_desc` / backbuffer descs.

## 3. Monitor + Present

- The real backbuffer is written only by the carrier: compose LEFT → real backbuffer (aspect-fit) in the Present path (device Present hook first; replace `LtiRenderer_Dx9_Present` if more control is needed).
- **Double-present retirement:** with the monitor composed by us, the second per-frame Present is a duplicate — skip it (keep one real Present per frame; device-lost detection rides on its HRESULT). Gate: the engine's own `g_SuppressPresent` (gates the Present in `LtiRenderer_BeginSubmit`, [reverse_engineering/dx9_state_wrapper.md](reverse_engineering/dx9_state_wrapper.md)) toggled per submit, or a skip in our Present hook. Halves compositor traffic; implements pacing option (c), an experiment rather than a proven fix ([pacing.md](pacing.md)). Verify once that a skipped fullscreen Present keeps the last frame.

## 4. Host

Blit 1:1 at swapchain size; keep aspect-fit letterbox for no-pose frames; `stretch=true` semantics retire with the squeeze ([host.md](host.md)). The per-eye projection (`vp_camera.cpp` `apply_eye`) is aspect-agnostic — no change.

## 5. Aspect / HUD policy (decide live)

The true eye aspect (0.9) is coherent for camera/culling consumers but untested in the engine's 4:3/16:9/letterbox UI modes (`FUN_004aefd0` pillarbox math). Snapped 4:3 is safe but squeezes the HUD ~0.68× horizontally. Try 0.9 first; HUD follow-ups in [hud.md](hud.md).

## Tracks

1. ~~RE: size authorities, reader classification, render target table~~ DONE 2026-11-07 (static).
2. Live instrumentation (`debug_rt_probe`): run 1 DONE 2026-11-08 (composite bind = baked op 6, depth mode, registries = name-keyed cache, boot order, creation census — [reverse_engineering/render_target_table.md](reverse_engineering/render_target_table.md)). OPEN: a menu resolution change (Reset path, aspect re-snap).
   - Lifecycle prerequisite (same run): the stage-2 device hooks went in at 28.595 s, while `SetupRenderTargets` was still running on the main thread (its tail fetched the front at 28.793 s). Device creation happens INSIDE Setup, so owning the table requires hooks installed at device creation — DONE 2026-11-08: the polling stage is gone, the device VmtHook is installed inside `CreateDevice` ([carrier.md](carrier.md)).
3. Output ownership at WINDOW size (behavior-neutral) — IMPLEMENTED 2026-11-08, `own_outputs=on` (`src/carrier/render_outputs.cpp`). Live run 3 (HMD, menus, resolution changes): takeover at boot and after both Resets, front binds land on the carrier surface, zero real-backbuffer binds in steady state, `failures=0`, swapchain without MSAA. Run 3 also surfaced a pre-existing bug: the view-rewrite pass gate cached the first backbuffer size, so after a resolution change no pass counted as main and stereo fusion broke. Fixed: it now keys on the live render size ([view_rewrite.md](view_rewrite.md)). Added after run 3: a retake MidHook at the CheckDevice restore tail (it was one frame late via the Present check). Run 4 CONFIRMED both: the view rewrite stays active at 1920×1080, the retake happens in the reset frame itself (`device restore`), there are zero real-backbuffer binds in every window including the resets, and `failures=0`.
   - Carrier front + depth copy the engine's descs (engine-requested MSAA) and replace every holder: table `+0x3ea4/+0x3e98/+0x3e94` (+`+0x3ea0` when aliased) and `g_Dx9AutoDepthRef`. Each owning slot is transferred (Release displaced, AddRef ours), mirroring the engine's own release accounting. The Dx9 bind caches are left alone (they mirror device state; our surfaces just miss once).
   - Takeover at the `SetupRenderTargets` exit (entry wrap, boot), at the `RenderSystem_CheckDevice` restore tail (Reset: setup is not re-run), and in a per-Present invariant check. Carrier refs are dropped at Reset; released pointers are never Released again.
   - Captures read the carrier front. The monitor is a LEFT mirror (front → mirror at 1→2, mirror → swapchain before every Present, aspect-fit). The pin is bypassed.
   - Present params: policy applied to a COPY (the engine's stored copy stays as requested — `RenderSystem_ApplySettings` compares it field by field); vsync interval as before, plus swapchain MSAA → none (moved to the carrier front).
   - Registry entries still name the swapchain surfaces (no observed consumer; the probe would show real-backbuffer binds).
   - DONE 2026-11-08 (after an `eye_rt=off` check run — both eyes and post effects correct): carrier-owned outputs are unconditional; the pin, the `eye_rt` redirect (+ the UpdateSurface/UpdateTexture hooks that only served it) and backbuffer capture are deleted; eye dumps read the mirror (LEFT) and a resolve of the front (RIGHT). `own_outputs`, `eye_rt` and `eye_monitor_pin` are retired conf keys (logged and ignored).
   - Double present: DONE 2026-11-08, live-confirmed (Present = frames exactly, e.g. 832/832 per 10 s; BeginScene 2/frame; presentSkips ≈ frames; `failures=0`; frame times 10.5–13.4 ms, unchanged or slightly better; host 120 Hz, clean). Track 3 is complete. Pass 1's present-prev is skipped through the engine's own `g_SuppressPresent` (`0x011755c3`; `PgPrimitive_SubmitToGPU` sets it only in its precache branch and clears it after a successful BeginSubmit). The carrier sets it before pass 1 and clears it again before pass 2; `outputs: window presentSkips=` counts the skips. Expected: `D3D: calls … Present≈frames`, and BeginScene still 2 per frame.
   - `pass_is_main` keys on the live render size, which is enough for decoupling since the render fields ARE the render size.
   - MSAA: not a regression. The PC port's in-game MSAA option never reaches the 3D scene (a known port bug; the scene renders into a non-MSAA fp16 texture, and only the backbuffer/composite/HUD carried the MSAA, which the carrier front keeps). Forcing MSAA in the driver reportedly works. Real scene MSAA would be its own feature: an MSAA scene RT + depth, resolved into the main texture before the first post read.
4. Render size — IMPLEMENTED 2026-11-08. Native run 1: world BLACK (fog + HUD visible). The setup chain (main RT, depth copy, scene depth, downsample) was still 2560×1440, because the engine sizes it from the front's GetDesc, not the fields. Fixed with a MidHook rewriting that desc (`0x0074a2c1`), plus a takeover-time WARNING if the main RT ever mismatches the render size. Run 1 also showed: HUD fine; menu mouse slightly off (OPEN below). Native run 2 pending: `render_size=WxH` (`src/carrier/render_size.cpp`, §1). The OnScreenResize entry/epilogue and setup-store MidHooks, aspect = true W/H (`render_aspect=true`, or `snap`), and mouse scale × render/cfg. A per-Present check logs field drift and re-applies aspect/mouse drift. The carrier front/depth/mirror are created from the render fields, so they follow; the monitor shows LEFT aspect-fit (pillarboxed). Default 2016×2240 = this HMD's OpenXR eye swapchain.
5. Aspect/HUD policy (§5).
   - Native run 2 (2026-11-08): world renders, sharper, fusion OK, third-person camera normal, main RT 2016×2240, no mismatch warnings. Gameplay frame time ~14–15 ms vs ~11–13 ms at window size (~+22% pixels; probe on).
   - **Render size follows the HMD** (default, `render_size_follow_hmd=on`, IMPLEMENTED 2026-11-08, pending live). The configured size applies only while the HMD is in use (session VISIBLE/FOCUSED + tracked, 1 s debounce); otherwise the game renders at window size exactly as unmodded, so the mouse needs no remapping. The switch reuses the engine's own device-lost recovery: `RenderSystem_CheckDevice` calls `TestCooperativeLevel` every frame, and our device hook answers it ONCE with DEVICENOTRESET. The engine then releases everything, Resets, runs `OnScreenResize` (our entry hook supplies the new target) and restores; we retake at the restore tail. RISK: the restore path's main-chain creation is inside the mutated `RenderSystem_RestoreDeviceResources`, sizing source unknown. If it sizes from the swapchain, the takeover WARNING fires and the world goes black after a switch to native; then that path needs its own fix.
   - Follow-HMD run (2026-11-08, 6 switches): works. The restore path re-enters the setup tail, so the chain-desc hook covers it (no WARNING; main RT 2016×2240 ↔ 2560×1440). Three notes, all addressed (pending live):
     - **Size from the runtime:** `render_size=hmd` uses the host's `MC2VR_MSG_VIEW_SIZE` (OpenXR recommended eye size) and always follows the HMD.
     - **Switch lag** (~1 s, my debounce): no carrier debounce at all now. "In use" = session VISIBLE/FOCUSED only, the runtime's lifecycle signal, which the runtime already debounces. The per-frame tracked flag is deliberately excluded: a brief tracking loss must not cost a device reset.
     - **Bright flash after each switch:** the Reset re-creates the HDR auto-exposure surfaces (`_AdaptiveLuminanceSurface` / `_pPrevAdaptiveLuminanceSurface`, 1×1 fp16, + `g_pAdaptiveDest`). Per frame, op `0x16` (= StretchRect, `0x00856c77`) copies prev → current, then the adaptation draw writes the new value into prev (prev = the persistent value). `src/carrier/adaptation.cpp` saves them (GetRenderTargetData → SYSTEMMEM) before the forced reset and UpdateSurfaces them back after it. Run 2 (diagnostic readback) showed the restore landing, but the FIRST post-reset frame's adaptation step dragged it to ~0 (0.257 → 0.000; ~zero measured luminance from the fresh chain + a reset-sized dt), followed by a slow climb. Fix: re-apply the saved values for the first 3 frames after the reset — live-confirmed, the flash is gone. Remaining: the reset frame itself came out fully white (its own adaptation step). That frame is now not shown: no eye capture (the host re-presents the previous pair) and a black monitor frame (`render_outputs` transition frame, armed at Reset END; `outputs: window transitionFrames=`; pending live).
   - OPEN (only relevant with `render_size_follow_hmd=off`): menu mouse mapping. The engine maps window mouse coordinates through `g_MouseScaleX/Y` (now render ÷ client), but the monitor shows the render image pillarboxed (aspect-fit), so the cursor is off by the bar offset and the non-uniform scale. Fix: map through the same fit rect (offset + uniform scale) — RE the WM_MOUSEMOVE path (`g_WindowOriginX/Y` subtraction, code near `0x006310e6`). Keyboard input works meanwhile.
6. Host 1:1 + acceptance: selftest; live run with `view: main scene RT size` / `eye:` logs at native, dump BMPs via `tools/analyze_dumps.py`, HUD, menu round-trip incl. a resolution change, mouse in menus, perf via `FrameTick: dt avg`.

## Rejected

- **Creation-call substitution** (device slots 23/28/29): viewports and screen constants stay config-sized; size keying is fragile.
- **Overriding `g_RenderCfg`** plus present-param rewrite: the window is created at cfg size, and ApplySettings' mode lookup fails and falls back to 640×480 (writing cfg).
- **Native-size swapchain:** present params are fullscreen, and 2016×2240 is no display mode.
- **Fields-only seam** (the 2026-10-11 plan): backbuffer-bound draws (composite, menus, movies) and the auto depth stay swapchain-sized.
- **Device-level virtual swapchain** (getter substitution + rebinding after Reset): works, but fakes values at the device layer and requires hooking the device at creation; superseded by table ownership. Kept as the backstop if a device-getter path to the backbuffer turns up.

## Risks

- Perf: 3.7 → 4.5 MP per pass (+22% pixels) on an already GPU-sync-bound loop ([pacing.md](pacing.md)); mitigation `render_scale`.
- Function replacement inherits the engine's register conventions — RE each callee before replacing; keep replacements small and log-instrumented.
- `reset_hook` counters are the canary for engine-internal rebuild loops; the fields-vs-cfg divergence is engine-internal only (ApplySettings never reads the fields).
