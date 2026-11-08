# Frame pacing

## The vsync lock (found, live-log-proven 2026-10-05)

The frame did two Presents — a present-prev at each `LtiRenderer_BeginSubmit` (each submit also waits on all prior GPU work: event-query spin). Vsync-locked 60 Hz capped the game at ~30 Hz with the two passes (~600 Presents vs ~300 frames per 10 s). Since 2026-11-08 pass 1's present-prev is skipped through the engine's own `g_SuppressPresent` ([render_outputs.md](render_outputs.md)), so there is one Present per frame.

## Fix: `vsync=off`

Force `D3DPRESENT_INTERVAL_IMMEDIATE` in the CreateDevice params: attach-time InlineHook of the game's `Direct3DCreate9` IAT thunk (`0x00a4e892`, single caller `RenderSystem_Init` `0x0074c7a0`) + VmtHook on the returned `IDirect3D9` (`CreateDevice` slot 16); the device-level `Reset` hook re-patches on Reset. Why at the D3D boundary: the engine's own CreateDevice call site is VM-gated (`FUN_0074c9b0` → ptr `0x024cd09c`, never hooked), and the carrier loads before the device exists ([carrier.md](carrier.md)). Conf key `vsync=off` (default off = hook not installed).

Live result: ~135 Hz gameplay (dt avg ~7.4 ms; pass 2 costs 3.2 ms — the wait was the cost, not the GPU), steady under full VR load; host ~120 Hz with `pose ids miss=0`. Adaptive-framerate bypass (`g_FrameratePolicy` / `AdaptiveFramerate_Govern`) stayed unneeded — no dt instability at ~135 Hz.

## Decision: FREE-RUN (closed 2026-10-06)

At ~135 Hz the game outpaces the ~120 Hz host; the host consumes the newest pair and the runtime reprojects.

## KNOWN ISSUE — head-turn judder (OPEN, fixes pending live verification)

Head turns feel choppy/low-rate while the frame rate is fine and the image is smooth with the head still. Error scales with head velocity → the image content and the pose it is submitted with disagree, or the pair is older than it needs to be. Loop audit 2026-11-08 found four defects, all fixed (verify with the next gameplay log):

1. Commands (FRAME_READY) were drained BEFORE `xrWaitFrame`, so a pair completed during the wait block slipped a frame — image age jittered between 1 and 2 periods with the game/HMD beat. Now late-latched after `xrBeginFrame` ([host.md](host.md)).
2. The render pose was predicted for the CURRENT frame's display time, though its image reaches the display ≥1 period later. Now predicted for `+1 period`. Evidence: `submit: … warp avg/max` (ms; ~0 ideal, ≈8.3 = one frame behind).
3. Eyes were taken newest-per-eye, so LEFT of frame N+1 could ship with RIGHT of frame N. Now complete pairs only (`seyes: … pairs/unpaired`).
4. The host mirror did a blocking full-res staging readback + GDI stretch of both eyes on every fresh frame inside the HMD loop. Now ≤5 Hz, non-blocking (`mirrorMax`).

Carrier side: the per-eye delta was taken against the CURRENT frame's camtable union, assuming the uploaded VP was built from this frame's fill. If the engine builds a VP from an earlier fill, the rendered eye ≠ the tagged pose by one frame of head motion — exactly this symptom. The delta now uses the union the VP actually carries (matched by basis, `camera_table::union_for_camera`); `camtable: VP union age 0=… 1=…` records which case the game is in (any non-zero ages = that was a live bug).

If judder remains after these: check SteamVR's frame timing for missed host frames; then (a) timewarp inputs (depth/motion vectors) or (b) throttle the game to HMD cadence via slot-4 `EndOfFrameHook` ([hooks.md](hooks.md)).
