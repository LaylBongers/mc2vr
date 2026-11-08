# Render resolution decoupling (window → HMD native)

Planned; revised 2026-11-07 (supersedes the 2026-10-11 field-seam-only plan), not implemented. Goal: per-eye passes render at the HMD's native resolution (2016×2240/eye, [openxr_proton.md](openxr_proton.md)) while the window, present params and swapchain stay stock (fullscreen 2560×1440).

RE basis: [reverse_engineering/rt_dims_source.md](reverse_engineering/rt_dims_source.md) (size authorities, readers, aspect snapping) and [reverse_engineering/render_target_table.md](reverse_engineering/render_target_table.md) (output surfaces, lifecycle).

## Principles

- **Change render size only, not display size.** The engine already keeps two size authorities: display = `g_RenderCfg` (window, present params, mode lookup, mouse) and render = RenderShell `+0x2b98/+0x2b9c`. Stock sets them equal. We change render only, so the swapchain, window, `Reset` behavior and `RenderSystem_ApplySettings` stay stock.
- **Own engine objects; don't intercept the device.** The carrier creates the output surfaces and puts them into the engine's render target table; size policy lives in a carrier-owned `OnScreenResize`. Device vtable hooks stay observe/forward plus Present composition. Device-level substitution (fake getters, per-call redirects) is a backstop only.
- **Replace renderer functions gradually where a seam is too thin** ([hooks.md](hooks.md)): start with the smallest plaintext function that owns the decision, reimplement it calling back into game code, and grow only where a seam proves insufficient. Candidates here, in order: `LtiRenderer_OnScreenResize`, the SetupRenderTargets plaintext tail, the Present path (`LtiRenderer_Dx9_Present`), and if needed the composite/EndSubmit stage.

## 1. Render size: carrier-owned `OnScreenResize`

- Replace `LtiRenderer_OnScreenResize` (0x007492f0, ~30 lines of plaintext; register args: W on the stack, H in EAX, `this` in ESI). Our version:
  - writes the fields, float copies and UI half-dims from the carrier render size;
  - sets aspect from carrier policy (§5) instead of 4:3/16:9 snapping;
  - sets `g_MouseScaleX/Y` = render ÷ client size, so the menu mouse maps into render space;
  - calls the engine's follow-ups (`FUN_0085a9b0` viewport, `FUN_00854da0`×2, `FUN_00609ef0` if `DAT_01174fb0`, `FUN_00709900` Bink rects).
  - Precondition: callee conventions RE'd. Fallback: rewrite the arguments at entry plus fix up at the epilogue.
- Every size path funnels through it: device create, device restore and menu resolution change all call `OnScreenResize(g_RenderCfg)`.
- SetupRenderTargets copies cfg → fields itself (0x007494fa..0x00749500): replace that copy, and re-apply aspect at its plaintext tail (the mutated stretch re-snaps it).
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
4. Render size: carrier `OnScreenResize` + Setup copy + invariant check; outputs follow at native.
5. Aspect/HUD policy (§5).
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
