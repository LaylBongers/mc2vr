# Render resolution decoupling (window → HMD native)

Planned 2026-10-11, not implemented. Goal: per-eye passes render at the HMD's native resolution (2016×2240/eye, [openxr_proton.md](openxr_proton.md)) while the game window, present params and swapchain stay untouched at 2560×1440. An earlier variant of this plan resized the swapchain backbuffer via the CreateDevice/Reset present-param seam — superseded at planning review: it changed what is presented into the window (forced-windowed + DXVK scaling assumptions).

## Mechanism: size-substituted render buffers (NOT a backbuffer resize)

The engine's render chain (G-buffers, fp16 main RT, post/tonemap targets, depth) must run at native size or the resolution is lost mid-chain (the composite/tonemap would downscale before capture). So: intercept the creation calls at the device vtable — `CreateTexture` (23), `CreateRenderTarget` (28), `CreateDepthStencilSurface` (29, per d3d9.h next to the live-verified 28/34/36 pins) — and substitute native-sized surfaces for backbuffer-derived requests. The engine receives its own (bigger) surface and the chain runs native; the swapchain backbuffer keeps the window size, so the composite landing on it is just a monitor downscale.

- Nothing about the device/swapchain/window changes ⇒ no DXVK scaling question, no forced-windowed, and engine-config vs device-state divergence stays zero (`Reset` behavior untouched).
- D3D9 Presents only ever reach the game window — HMD delivery stays the shared-texture path ([shared_textures.md](shared_textures.md)). "Backbuffers for VR" here means backbuffer-ROLE offscreen surfaces (the eye RTs), never the swapchain; the swapchain is the monitor mirror and nothing else.
- The per-eye projection (`src/carrier/vp_camera.cpp` `apply_eye`) is aspect-agnostic (eye FOV tangents → whatever buffer exists). At the native aspect the render-target squeeze ([host.md](host.md) stretch=fill is its inverse) retires with no projection change.
- Engine CPU-side dims (viewport/scissor/HUD layout/screen-size constants) may be desc-driven (self-adapts to the substituted size) or config-driven (needs rescale hooks) — census decides: [reverse_engineering/rt_dims_source.md](reverse_engineering/rt_dims_source.md).

## Stereo pipeline restructure (capture at native)

Today LEFT is captured from the backbuffer (pass-1 composite draws into RT0=backbuffer), RIGHT from the carrier eye RT — with a window-sized backbuffer that would lose LEFT's native pixels. So:

- Pass 1: redirect the composite's `SetRenderTarget(0, backbuffer)` to a native LEFT eye RT (same mechanism as the existing pass-2 redirect, now needed on both passes).
- Capture both eyes from the eye RTs (1→2 LEFT, 2→0 RIGHT); shared ring slots at native size (desc-derived, follows automatically).
- Monitor: explicit downscale blit LEFT eye RT → backbuffer after the pass-1 capture; the monitor-pin snapshot/restore retires (pass 2 no longer touches the backbuffer at all — the pin's stale-page problem [stereo.md](stereo.md) disappears with it).
- `view_rewrite::pass_is_main` gates on RT0 size == backbuffer size — under decoupled sizes it would SILENTLY STOP rewriting the main pass; re-key on RT identity (its own code comment prescribes exactly this).
- Carrier eye RT is created from `g_main_desc` (`eye_replay.cpp` `ensure_eye_rt`) — follows the substituted main RT without changes.

## Double-present retirement (falls out of the same restructure)

With the backbuffer reduced to a pure monitor mirror (written only by our downscale blit at 1→2), the present-prev at each `BeginSubmit` is a DUPLICATE on the first of the two: pass-2's Present shows frame N's LEFT, and frame N+1's pass-1 Present shows the same content again. Suppress the redundant one — either the engine's own `g_SuppressPresent` (gates only the Present inside `LtiRenderer_BeginSubmit`, [reverse_engineering/dx9_state_wrapper.md](reverse_engineering/dx9_state_wrapper.md); toggle per-submit from the existing dormant `BeginSubmit` MidHook) or a skip in the Present VmtHook keyed on pass state. Monitor keeps full-rate fresh LEFT; halves window compositor traffic; retires the deferred double-Present gap ([stereo.md](stereo.md)). Caveats: keep one real Present per frame (device-lost detection rides on its HRESULT); verify once that a skipped windowed Present leaves the compositor-retained frame (the stale-page lesson was suppressed backbuffer WRITES, a different mechanism). Also implements pacing option (c) — a measurable staleness experiment, not a proven fix ([pacing.md](pacing.md)).


## Tracks

1. Surface census + dims-source RE: [reverse_engineering/rt_dims_source.md](reverse_engineering/rt_dims_source.md) — decides the substitution keying (which creation requests count as backbuffer-derived) and whether the engine self-adapts to desc sizes.
2. Creation-substitution hooks (device slots 23/28/29) + viewport/scissor rescale hooks if the census demands; watch for size-keyed caller-side cache mis-hits ([reverse_engineering/dx9_state_wrapper.md](reverse_engineering/dx9_state_wrapper.md) — creation calls return the substituted pointer, which the caches store as-is).
3. Stereo restructure (pass-1 composite redirect, native captures, monitor downscale blit, pin retirement, `pass_is_main` re-key, double-present retirement above).
4. Host: 1:1 blit at swapchain size; keep the aspect-fit letterbox path for no-pose frames; retire stretch semantics.
5. Acceptance: selftest, live run — `view: main scene RT size` / `eye: created eye RT` logs at native, dump BMPs via `tools/analyze_dumps.py`, HUD state, menu round-trip, perf via `FrameTick: dt avg`.

## Risks

- Perf: 3.7 → 4.5 MP per pass (+22% pixels) on an already GPU-sync-bound loop ([pacing.md](pacing.md)); mitigation: `render_scale` conf (substitute a smaller native-scaled size, submit into the same swapchain).
- Screen-space post effects whose CPU-side pixel constants (bloom offsets, texel steps) are computed for 2560×1440 would mis-sample native buffers → soft/incorrect post; pixel-size constant census in the RE note, rewrite at the upload gate if found ([reverse_engineering/shader_ctab_map.md](reverse_engineering/shader_ctab_map.md)).
- Substitution keying too broad/too narrow: non-chain surfaces (shadow maps, light scopes) must NOT be resized; every chain surface MUST be, or the chain mixes sizes mid-way.
- HUD/menu layout: engine lays out 16:9; stretched into a 9:10 buffer — same distortion class as today's squeeze, no worse; [hud.md](hud.md).
- Engine desc-readback paths seeing native sizes where its config says window — `reset_hook` counters are the canary for engine-internal rebuild loops (device state itself is untouched, so the divergence risk is engine-internal only).
