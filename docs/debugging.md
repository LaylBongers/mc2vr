# Debugging / live runs

## Iteration loop

The agent implements/logs; the human runs `./launch.sh` into GAMEPLAY (SteamVR up — gameplay, not menu, for render instrumentation) and reports; the agent audits `<GAME_DIR>/mc2vr/mc2vr_{carrier,launcher}.log` (`GAME_DIR` from `launch.conf`). `tools/analyze_dumps.py <log>` parses view/stream/eye evidence (view dumps need `debug_view_dumps=on`).

Render-thread timings use integer QPC ticks (`src/carrier/qpc.hpp`); census CPU times logged before 2026-11-10 were quantized to 4 ms steps ([x87_precision.md](x87_precision.md)).

## Vulkan validation

`MC2VR_VK_VALIDATION=1 ./launch.sh`: the Khronos validation layer with synchronization validation in the game's Vulkan stack, uncapped, logged to `<GAME_DIR>/mc2vr/mc2vr_vk_validation.log`. Needs the 32-bit layer (`lib32-vulkan-validation-layers`): the game's Vulkan calls go through Proton's i386 Unix side. ~17 Hz in the HMD, so keep runs short. DXVK produces ~2 messages per frame of its own; ours are found by the handles the carrier logs ([xr_session.md](xr_session.md#live-results)).

## Deployed conf steady state

`render_size=hmd vsync=off` (the VR pipeline itself has no keys — it is always on and passes through without an OpenXR session). The deployed `mc2vr.conf` is never overwritten by `launch.sh` — edit it in place.

## Healthy-run signatures

Session ([xr_session.md](xr_session.md#live-results)): `xr: session created on DXVK's device`, `xr: swapchains … format 50`, `submit:` ~900 frames per 10 s at 90 Hz with `shown` ≈ frames and `noPair noPose copyFail layerRejected` all 0 outside Resets. Eye images: `share window:` pairs ≈ frames, `flush` avg ~1 ms. `view/hmd: split=0 decompFail=0`. Game with `vsync=off` and no HMD: `FrameTick: dt avg` ~7.4 ms (~135 Hz).

## Phase oracles (reading a run's log)

- `view: record` census: menu = idx 2/3 (immediate after attach), load-in = idx 9/10 (~+13 s), gameplay = idx 4,5,6,7,8,12,13 (~+20 s) — the world-scale set (4/5/6) appearing at all is the "truly in-world" signal.
- The menu/loading vista camera parks at ~(-1730,-33,2064) with slow rotation — a decomposed `camC` frozen there across a run means the world pass never engaged, regardless of what was on screen.
- `view/hmd: blocks=0` + `poseId=0` for whole windows = the HMD was never tracked (everything downstream of the hmd rewrite's decompose — matching, `get_game_camera` — is blind without it).
- `camtable: camera frame MEASURED` = the row-sign calibration locked (expected row0.R=−1, row1.U=+1, row2.F=+1; a different result means the convention changed — re-run the probe before trusting the union pose: [camera.md](camera.md)).
- When a run "does nothing", first check that the conf took effect in the log (the round-8 `bool` vs `float` no-op).

## Per-eye dump-pair measurement recipe (learned 2026-10-07 — do NOT re-derive)

1. Phase-correlate the pair for the GLOBAL offset (the frustum-center offset is ~−450 px at 1440p — a ±64 px search window reports "no parallax" wrongly).
2. Fit the per-eye horizontal scale (kx ≈ 0.983 in the reference run) from per-eye FOV spans.
3. Measure parallax of target strips vs a **true far reference (sky)** — a wall coplanar with the target gives ~0 parallax and nonsense distances. Convert with `px_per_tan = W_px / (tan(fov.right) − tan(fov.left))` per eye (from the `ipc: first tracked pose ... fovL=` log line), `z = ipd·s·px_per_tan / parallax`.
4. Backlog: `tools/analyze_dumps.py` still does horizontal-only ±64 px SAD — upgrade it with this recipe. Interim check (carrier-owned LDR finals, central band, Hann-windowed 2D phase correlation): global L-vs-R offset +481…+508 px, dy = 0, across 3 gameplay pairs. That is consistent with the ~450–460 px frustum-center offset plus scene parallax; the sign follows the correlation convention.

## Crash reports

- **Carrier crash logger (always on, `src/carrier/crash_log.{hpp,cpp}`).** An unhandled-exception filter, installed at attach, writes `CRASH:` lines to the carrier log:
  - the exception and its address (module+offset), access details, registers;
  - the code bytes at EIP;
  - the thread (`(update thread)`) and the frame;
  - executable-memory values in the first 1 KB of stack (candidate return addresses).

  Then it chains to the replaced filter. It also switches Wine's crash dialog off (`SEM_NOGPFAULTERRORBOX`). That dialog opens behind the fullscreen game and holds the process, so a crash looked like a freeze; now the game exits after the report. The 10 s poller puts it back on top if the game installs its own (`crash: filter had been replaced…`). Each distinct exception is reported once (Wine calls the filter again after its default handler). Only exceptions nobody handled reach it, never SecuROM's or the game's first-chance ones. Test: `tools/test/test_crash_log.cpp` (`mc2vr_crash_log_test`, run under Wine).
- **Hang watchdog (always on, same file):** if the frame counter stops for 20 s, a `HANG:` report gives the update thread's registers and a stack scan, captured while it's briefly suspended. `HANG: frames resumed after N s` follows if it recovers. A very long synchronous load can trip it; the report then simply shows the loader. Not yet seen live.
- **Proton log (opt-in):** `MC2VR_PROTON_LOG=1 ./launch.sh` writes `steam-<id>.log` to the deploy dir, with Wine's view of the crash, loader errors and the default debug channels. With `debug_watch` on it grows fast (`+seh` logs every first-chance exception, including the watch traps).
- Reading a report: the address is `module+offset`. Add `0x00400000` for the game's Ghidra VA, or match `code at EIP` against `read-memory` / host disassembly. A crash inside `Stext`/`.securom` is protected code; look at the stack's game return addresses for the plaintext caller.
- A crash that turns out to be a game bug gets a hotpatch through the existing mechanisms ([hooks.md](hooks.md)): a guarding MidHook at the faulting site, or a reimplementation of the function ([reimpl.md](reimpl.md)). Record the bug and the fix in the note of the system it's in.
- Unexplained crash (no report yet): 2026-11-11, ~12:14:40, on foot, ~85 s after a car → ragdoll → human camera sequence. It ran with `debug_gameplay_census` + `debug_watch` + `debug_occlusion_off`, before the logger existed. The OpenXR session had dropped to SYNCHRONIZED at 12:14:21, ~17 s earlier, with watchpoints armed. That's the same pattern as the earlier one-time crash in [culling.md](culling.md#open-items) (session drop + `debug_watch`, ~20 s). Suspect: the watch together with a session drop.

## Hard-won gotchas

- Raw byte decode beats decompiler output in SecuROM-mutated regions: the decompiler constant-folds memory loads and shifts stack offsets (e.g. `stack0x24` = raw `[esp+0x30]` in `ViewContext_BuildCameraConstants`). Verify hook sites against raw bytes and hook only exact instruction boundaries — one byte off crashes at boot.
- `debug_watch` on a struct's first fields catches block copies (`rep movsd`, ESI just past the watched dword) — that is how the camera-clearance consumer was found after static xref hunting came up empty.
- Multi-DR watchpoint attribution under Wine is unreliable (`field=?`); EIPs and registers are still exact, which is enough to identify readers.
- Never trust value snapshots of stack addresses outside the owning call; same-EIP aggregates hide multiple callers (`Matrix_Copy3x4`'s 131 call sites all watch as one EIP).
- Measure transfer functions instead of guessing sign conventions — [camera.md](camera.md) § Entry convention.

## Debug tools (carrier `debug_*` conf keys, default off — [carrier.md](carrier.md))

`debug_occlusion_off` (engine occlusion culling off, for A/B — [occluder_boxes.md](occluder_boxes.md)), `debug_build_census` (scene → draw-record build: threads, timing, task order, records per emitter/sub-view/type — [reverse_engineering/scene_record_build.md](reverse_engineering/scene_record_build.md)), `debug_vm_dump` (live thunk slot census → `vm_thunks.csv`; a 2026-10-03 copy at `reverse_engineering/data/vm_thunks_runtime.csv`), `debug_watch` (hardware watchpoints, `addr:0x…`, or `player:<hex>` fields of the player object), `debug_gameplay_census` (first-person runtime facts: frame phases, bone-controller callers, camera classes, player modes, look input — [first_person.md](first_person.md)), `debug_camtable_probe` (camera transfer-function probe — [camera.md](camera.md)), `debug_eye_dump_frames` (per-eye BMPs). `debug_gui_census` (draw-time view of the GUI records — [hud.md](hud.md)). Removed after their tracks closed (git history): `debug_xr_probe` (in-process OpenXR feasibility, [xr_session.md](xr_session.md)), `debug_sync_frame_ready`, `debug_pair_gpu_wait` (the hand-off A/B, [xr_session.md](xr_session.md#live-results)), `debug_terrain_cull_probe`, `debug_zstate_probe`, the E2/E2b inject_probe module, `debug_stub_trace` ([reverse_engineering/vm_stub_callbacks.md](reverse_engineering/vm_stub_callbacks.md)), `debug_rt_probe` ([render_resolution.md](render_resolution.md)), `debug_hud_probe` (minimap widget fields and per-caller `Gui_DrawTexturedRect` rects, found the border's sizing — [hud.md](hud.md)), `debug_msaa_census` ([msaa.md](msaa.md)), the SecuROM pre-probes.
