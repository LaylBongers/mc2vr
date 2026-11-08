# Frame pacing

## `vsync=off`

The engine Presents once per frame (pass 1's present-prev is skipped through its own `g_SuppressPresent`, [render_outputs.md](render_outputs.md)); with vsync on, the desktop refresh would cap the game. `vsync=off` forces `D3DPRESENT_INTERVAL_IMMEDIATE` in the CreateDevice params: attach-time InlineHook of the game's `Direct3DCreate9` IAT thunk (`0x00a4e892`, single caller `RenderSystem_Init` `0x0074c7a0`) + VmtHook on the returned `IDirect3D9` (`CreateDevice` slot 16); the device-level `Reset` hook re-patches on Reset. Done at the D3D boundary because the engine's own CreateDevice call site is VM-gated (`FUN_0074c9b0` → ptr `0x024cd09c`) and the carrier loads before the device exists ([carrier.md](carrier.md)). Default off = hook not installed.

## Lockstep with the host

The host runs the OpenXR app loop with the game as the renderer: `xrWaitFrame` → `xrBeginFrame` → locate + publish the pose for that frame's `predictedDisplayTime` → wait for the pair rendered from it → submit it in the same `xrEndFrame` ([host.md](host.md)). One game frame per host frame, no re-submits: a game slower than the display makes frames late, and the runtime's own timing and throttling handle it. (Free-running and re-submitting the newest pair hid the game's real cadence and latency from the runtime.)

Carrier side, `src/carrier/pacing.cpp`, at the `GameShell_FrameTick` entry hook while the session is VISIBLE/FOCUSED:

- **Gate:** the frame starts only once the host has published a pose newer than the one the last frame rendered with. The pass-1 pose sample then reads that publish.
- **Simulation dt = display delta:** `g_FrameDeltaSec` is overwritten with `predictedDisplayTime[n] − [n−1]`. The engine's own value is the FrameTick *body* time, not the frame interval, so any wait outside it (the gate) desyncs the simulation clock from the display; its dt governor can also quantize to whole frames of a target rate with a strict `>` (plates on `0x01175048`, `0x004c1690`, `0x004f5720`). In that quantizing mode the fed value is 0.25 ms short so the governor rounds to the true frame count. Time scale, pause and clamps still apply.

Both waits are bounded so neither side can hang the other: carrier gate 50 ms, host pair wait 100 ms (the host only waits while pairs arrived in the last 500 ms). Log lines: `pace:` (gate waits, fed dt vs `g_Dt`, governor mode/rate), `submit:` (fresh/late/stale, wait times).

The stereo replay pass skips `LtiRenderer_BeginSubmit`'s per-submit GPU drain (MidHook on its drain check `0x0074ab5f`), so the CPU queues the right eye while the GPU still draws the left; DXVK's own hazard tracking covers buffer reuse. `debug_keep_replay_drain=on` restores the drain.

Head-pose consistency of the rendered pair: [camera.md](camera.md).

## Half rate = double image on moving objects

Below the display rate SteamVR runs the game at half rate (60 on a 120 Hz panel) and flashes each frame twice; moving objects the eyes follow then show a back-and-forth double image (desktop monitors smear instead, so the mirror looks clean). Confirmed: at 20% render resolution and 80 Hz the game ran near full rate and the double image was gone. SteamVR motion smoothing is not available on Linux, so the fix is frame rate.

## OPEN — frame-time headroom

At HMD render size a frame takes ~11.5 ms (needs 8.3 at 120 Hz, 11.1 at 90). The engine does not limit the frame rate (governor mode 0 `Variable`, no `Sleep(n)`/QPC spin, `debug_wait_census`); the time is the render worker plus the GPU: prep ~2.6 ms → pass 1 ~1.8 → pass 2 ~1.6 → end-of-frame GPU sync ~3–4.5, while the update thread waits for the render task ([render_threading.md](reverse_engineering/render_threading.md)). Lower resolution barely helps (~10–11 ms at 20%). Next lever: gate the render thread's pose sample instead of the update thread, so the next frame's update and render prep overlap the host wait — possible now that the simulation step comes from display time.

Remaining hitches at 80 Hz: missed frames (~6% 25 ms steps — headroom), and rare 100 ms host waits for a pair (cause unknown; render-thread stall such as a DXVK shader compile is a candidate).

The game's camera motion blur (`Mercs2.ini MotionBlur`) smears moving objects in VR.
