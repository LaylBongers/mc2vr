# Frame pacing

## The vsync lock (found, live-log-proven 2026-10-05)

The frame did two Presents — a present-prev at each `LtiRenderer_BeginSubmit` (each submit also waits on all prior GPU work: event-query spin). Vsync-locked 60 Hz capped the game at ~30 Hz with the two passes (~600 Presents vs ~300 frames per 10 s). Since 2026-11-08 pass 1's present-prev is skipped through the engine's own `g_SuppressPresent` ([render_outputs.md](render_outputs.md)), so there is one Present per frame.

## Fix: `vsync=off`

Force `D3DPRESENT_INTERVAL_IMMEDIATE` in the CreateDevice params: attach-time InlineHook of the game's `Direct3DCreate9` IAT thunk (`0x00a4e892`, single caller `RenderSystem_Init` `0x0074c7a0`) + VmtHook on the returned `IDirect3D9` (`CreateDevice` slot 16); the device-level `Reset` hook re-patches on Reset. Why at the D3D boundary: the engine's own CreateDevice call site is VM-gated (`FUN_0074c9b0` → ptr `0x024cd09c`, never hooked), and the carrier loads before the device exists ([carrier.md](carrier.md)). Conf key `vsync=off` (default off = hook not installed).

Live result: ~135 Hz gameplay (dt avg ~7.4 ms; pass 2 costs 3.2 ms — the wait was the cost, not the GPU), steady under full VR load; host ~120 Hz with `pose ids miss=0`. Adaptive-framerate bypass (`g_FrameratePolicy` / `AdaptiveFramerate_Govern`) stayed unneeded — no dt instability at ~135 Hz.

## Decision: FREE-RUN (closed 2026-10-06)

At ~135 Hz (window render size — at HMD size it is ~71 Hz, see below) the game outpaces the ~120 Hz host; the host consumes the newest pair and the runtime reprojects.

## Head-turn judder — FIXED (live-verified 2026-11-08, "very strong improvement")

Symptom: head turns choppy while frame rate fine and still-head image smooth. Root cause (carrier): the engine builds each frame's VP from the PREVIOUS frame's camtable fill (`camtable: VP union age` = 100% age 1, [camera.md](camera.md)), but the per-eye delta was taken against the current frame's union — the rendered eye was one game frame of head motion off the tagged pose, uncorrectable by the compositor. Fix: the delta uses the union the VP actually carries (`camera_table::union_for_camera`, matched by basis).

Host loop defects fixed in the same audit ([host.md](host.md)): FRAME_READY drained before `xrWaitFrame` (now late-latched after `xrBeginFrame`); render pose predicted for the current frame (now `+1 period`); per-eye newest images mixing two game frames (now complete pairs only, `unpaired=0`); blocking full-res mirror readback in the HMD loop (now ≤5 Hz non-blocking, `mirrorMax` ~2 ms).

## OPEN — residual warp at HMD resolution

At HMD render size (2016×2240/eye, two passes) gameplay runs ~71 Hz (dt avg ~14 ms), NOT the ~135 Hz measured at window size — the `+1 period` lead assumes a game faster than the HMD. `submit: warp avg` 10–13 ms, max 25 ms; fresh pairs on ~60% of host frames. Next: (1) adaptive lead — predict the published pose by the measured warp (EMA) instead of a fixed period; (2) profile the 71 Hz (GPU at HMD res?) and decide between speed-up and throttling the game to a stable HMD divisor via slot-4 `EndOfFrameHook` ([hooks.md](hooks.md)); (3) timewarp inputs (depth/motion vectors) for the translational residue.
