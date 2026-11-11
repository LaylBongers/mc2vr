# Frame pacing

## No vsync

The engine Presents once per frame (pass 1's present-prev is skipped through its own `g_SuppressPresent`, [render_outputs.md](render_outputs.md)); with vsync on, the desktop refresh would cap the game and stall the HMD, which the OpenXR runtime paces. The carrier always forces `D3DPRESENT_INTERVAL_IMMEDIATE` in the CreateDevice params: attach-time InlineHook of the game's `Direct3DCreate9` IAT thunk (`0x00a4e892`, single caller `RenderSystem_Init` `0x0074c7a0`) + VmtHook on the returned `IDirect3D9` (`CreateDevice` slot 16); the device-level `Reset` hook re-patches on Reset. Done at the D3D boundary because the engine's own CreateDevice call site is VM-gated (`FUN_0074c9b0` → ptr `0x024cd09c`) and the carrier loads before the device exists ([carrier.md](carrier.md)). Without a headset session the game runs uncapped and the monitor window may tear.

## Lockstep with the OpenXR frame loop

The OpenXR frame is carried by the game's stereo frame: the frame thread's `xrWaitFrame` → `xrBeginFrame`, the render thread's views at its `predictedDisplayTime` → both eye passes → copy, the frame thread's `xrEndFrame` ([xr_session.md](xr_session.md#frame-loop)). One game frame per OpenXR frame, and a frame exists only when the game renders one: no re-submits, so a game slower than the display makes frames late and the runtime's own timing and throttling handle it. (Free-running and re-submitting the newest pair hid the game's real cadence and latency from the runtime; the separate host and session thread still re-submitted on their timeouts.)

Carrier side, `src/carrier/vr/pacing.cpp`, while the session is VISIBLE/FOCUSED:

- **The runtime paces the render thread** through the hand-off: SteamVR throttles inside `xrEndFrame` on the frame thread, and the render thread waits for the next begun frame at its next frame start (`frameWait`), after Present and the next scene build. Keeping the throttle off the render thread keeps the scene build out of the pose → deadline window ([xr_session.md](xr_session.md#frame-loop)). The update thread is never held; it runs about a frame ahead ([render_threading.md](reverse_engineering/render_threading.md)). `pacing::on_frame` takes each shown frame's display time and period from the frame itself.
- **Simulation step = display time:** at the `GameShell_FrameTick` entry hook (update thread, before the game's QPC sample) `g_FrameDeltaSec` is overwritten so the simulation advances one display period per frame (the runtime's `predictedDisplayPeriod`, published with the views — exact when SteamVR changes rate), and catches up when the last rendered display time shows the display got ahead (missed frames). The update usually runs a frame ahead of the render thread's frame start ([render_threading.md](reverse_engineering/render_threading.md)), so "previous target + period" is the normal case (`floored` in the `pace:` line), not a fallback. The governor still applies its own processing to the fed value (mode 0: clamp to [0.1, 100] ms, × time scale, minimum 5 ms, zero when paused — no smoothing). The engine's own value is the FrameTick *body* time, not the frame interval, so any wait outside it desyncs the simulation clock from the display; its dt governor can also quantize to whole frames of a target rate with a strict `>` (plates on `0x01175048`, `0x004c1690`, `0x004f5720`) — in that mode the fed value is 0.25 ms short so the governor rounds to the true frame count. Time scale, pause and clamps still apply.

Log lines: `pace:` (frames paced / outside the loop, fed dt vs `g_Dt`, governor mode/rate), `submit:` (frames shown, per-call times).

The stereo replay pass skips `LtiRenderer_BeginSubmit`'s per-submit GPU drain (a condition at the drain check in our `LtiRenderer_BeginSubmit`, [reimpl.md](reimpl.md)), so the CPU queues the right eye while the GPU still draws the left; DXVK's own hazard tracking covers buffer reuse.

Head-pose consistency of the rendered pair: [camera.md](camera.md).

## Half rate = double image on moving objects

Below the display rate SteamVR runs the game at half rate (60 on a 120 Hz panel) and flashes each frame twice; moving objects the eyes follow then show a back-and-forth double image (desktop monitors smear instead, so the mirror looks clean). Confirmed: at 20% render resolution and 80 Hz the game ran near full rate and the double image was gone. SteamVR motion smoothing is not available on Linux, so the fix is frame rate.

## Frame-time headroom

At full HMD render size (2016×2240 per eye) the game holds 90 Hz on the Index: 899–901 frames per 10 s, about one late frame per 10 s in heavy scenes (frame-thread design, 2026-11-10). The engine does not limit the frame rate (governor mode 0 `Variable`, no `Sleep(n)`/QPC spin, `debug_wait_census`).

Budget per 11.1 ms frame (2026-11-10):
- Render thread: busy ~8.5 ms (11.1 minus `frameWait` ~2.6): Present, the next scene build (`RenderShell_BuildSceneRecords`, 2–4.4 ms, [reverse_engineering/scene_record_build.md](reverse_engineering/scene_record_build.md)), the two passes, the flush (~1 ms) and our copy. Draw submission, exact: main scene 1.31–1.41 ms in pass 1 and 2.16–2.33 ms in pass 2 for the same ~2,000–2,170 draws, ~0.3 ms per pass for the shadow atlas. Pass 2 is slower although it skips the HUD: it skips the per-submit GPU drain, so it runs into DXVK's back-pressure (its CS thread behind, the game thread waiting).
- GPU: ~6.7 ms of work (DXVK HUD `gpuload` ~60%, 50–75% → 5.6–8.3 ms; `DXVK_HUD=gpuload,frametimes ./launch.sh`, shown on the monitor only; DXVK's own work, without SteamVR's compositor).

So at 90 Hz both have room. For 120 Hz (8.33 ms) the render thread is the first wall (~2–3 ms too much) and busy-scene GPU peaks the second. 120 Hz run (2026-11-10, same settings, 2016×2240): a light scene (menu/loading, flush ~0.2 ms) held 120 Hz (1,198 frames per 10 s) with every frame on time and `frameWait` ~7.4 ms; gameplay dropped to 73–81 frames per second, with SteamVR mixing full, half and third rate (`predictedDisplayPeriod` read 25 ms at times, prediction depth all over 1–4+). The render thread was busy ~9.5 ms per actual frame (13.1–13.8 ms interval minus `frameWait` 3–4 ms), above the 8.33 ms budget, but that includes the flush and pass 2's back-pressure, which grow when the GPU is behind; the run doesn't separate CPU from GPU. A run at a lower render size (GPU work scales, render-thread CPU mostly doesn't) or with `DXVK_HUD=gpuload` would. CPU levers by size: running the scene build concurrently with the previous frame's submission (~3 ms; the draw records are double-buffered for it), single-pass stereo by draw doubling (~1.5–2 ms: pass 2's submission and back-pressure), the flush (DXVK's CS lag, shrinks with both). The eye-independent offscreen passes in the replay cost only ~0.35 ms GPU + ~0.35 ms CPU, so skipping them is not worth it. Occlusion culling (per-eye since 2026-11-10) bought ~0.4 ms. Headroom is also what would allow reading the pose later in the frame safely ([xr_session.md](xr_session.md#live-results)).

Reading `debug_gpu_census`: its CPU times are exact since 2026-11-10 (integer ticks, [x87_precision.md](x87_precision.md)). Its GPU figures are spans of the GPU timeline between timestamps, not busy time: DXVK submits in batches, so time the GPU waits for commands lands in whichever segment is open (menu frames with 1–356 draws show a 7.8–9.8 ms pass-2 "main" span), and the spans always add up to the frame period. That also explains the old pass-1-vs-pass-2 GPU difference (pass 1 "1.1 ms slower" in one run, pass 2 ~2 ms "slower" in the next): idle attribution, not first-pass cost. Earlier figures quoted from it ("~7.9 ms of GPU work") were spans.

Logging from game threads is expensive in bulk (bulk dumps from the game thread caused visible stalls): keep per-frame paths log-free. The logger doesn't disk-sync per line.

The game's camera motion blur (`Mercs2.ini MotionBlur`) smears moving objects in VR.
