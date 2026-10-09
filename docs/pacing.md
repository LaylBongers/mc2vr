# Frame pacing

## The vsync lock (found, live-log-proven 2026-10-05)

The frame does two Presents — a present-prev at each `LtiRenderer_BeginSubmit` (each submit also waits on all prior GPU work: event-query spin). Vsync-locked 60 Hz capped the game at ~30 Hz with the two passes (~600 Presents vs ~300 frames per 10 s).

## Fix: `vsync=off`

Force `D3DPRESENT_INTERVAL_IMMEDIATE` in the CreateDevice params: stage-1 InlineHook of the game's `Direct3DCreate9` IAT thunk (`0x00a4e892`, single caller `RenderSystem_Init` `0x0074c7a0`) + VmtHook on the returned `IDirect3D9` (`CreateDevice` slot 16); the device-level `Reset` hook re-patches on Reset. Why at the D3D boundary: the engine's own CreateDevice call site is VM-gated (`FUN_0074c9b0` → ptr `0x024cd09c`, never hooked), and the carrier loads before the device exists ([carrier.md](carrier.md)). Conf key `vsync=off` (default off = hook not installed).

Live result: ~135 Hz gameplay (dt avg ~7.4 ms; pass 2 costs 3.2 ms — the wait was the cost, not the GPU), steady under full VR load; host ~120 Hz with `pose ids miss=0`. Adaptive-framerate bypass (`g_FrameratePolicy` / `AdaptiveFramerate_Govern`) stayed unneeded — no dt instability at ~135 Hz.

## Decision: FREE-RUN (closed 2026-10-06)

At ~135 Hz the game outpaces the ~120 Hz host; the host consumes the newest pair and the runtime reprojects.

## KNOWN ISSUE — reprojection staleness (must be fixed eventually)

Apparent stutter/micro-judder during head motion that vanishes when the head is still. NOT frame drops (frame timing steady, dt min 5.8–7.8 ms) — the OpenXR compositor extrapolates the newest submitted pair (0–7 ms old at ~135 Hz game / ~120 Hz host), and the error scales with head velocity. The single biggest remaining VR-experience defect. Fix options, in preference order:

(a) **timewarp inputs** — feed motion vectors / late-stage data via slot-4 `EndOfFrameHook` ([hooks.md](hooks.md)) so the compositor reprojects accurately instead of extrapolating;
(b) **throttle the game to HMD cadence** (slot-4) so pairs are always fresh at submit time;
(c) suppress the deferred second Present ([stereo.md](stereo.md) gaps) if compositor pressure is implicated.
