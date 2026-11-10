# Debugging / live runs

## Iteration loop

The agent implements/logs; the human runs `./launch.sh` into GAMEPLAY (SteamVR up — gameplay, not menu, for render instrumentation) and reports; the agent audits `<GAME_DIR>/mc2vr/mc2vr_{carrier,host,launcher}.log` (`GAME_DIR` from `launch.conf`). `tools/analyze_dumps.py <log>` parses view/stream/eye evidence (view dumps need `debug_view_dumps=on`). Selftest needs an unsandboxed terminal: [build.md](build.md).

## Deployed conf steady state

`render_size=hmd vsync=off` (the VR pipeline itself has no keys — it is always on and passes through without a host). The deployed `mc2vr.conf` is never overwritten by `launch.sh` — edit it in place. Env: `MC2VR_NO_HOST`, `MC2VR_IPC_NAME`.

## Healthy-run signatures

Host: `submit: blit shaders ready`, `openxr: using swapchain format 91`, `submit: window fresh/reused/pattern=0`, `submit: pose ids miss=0`. Carrier: `share window:` ~1330 L/R per 10 s with `ringFull=0` (≈ game fps × 10), `view/hmd: split=0 decompFail=0`. Game with `vsync=off`: `FrameTick: dt avg` ~7.4 ms (~135 Hz). Pulsing test pattern in the HMD = the carrier pipeline is not talking (`eye_share=off` or the host is not receiving).

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

## Hard-won gotchas

- Raw byte decode beats decompiler output in SecuROM-mutated regions: the decompiler constant-folds memory loads and shifts stack offsets (e.g. `stack0x24` = raw `[esp+0x30]` in `ViewContext_BuildCameraConstants`). Verify hook sites against raw bytes and hook only exact instruction boundaries — one byte off crashes at boot.
- `debug_watch` on a struct's first fields catches block copies (`rep movsd`, ESI just past the watched dword) — that is how the camera-clearance consumer was found after static xref hunting came up empty.
- Multi-DR watchpoint attribution under Wine is unreliable (`field=?`); EIPs and registers are still exact, which is enough to identify readers.
- Never trust value snapshots of stack addresses outside the owning call; same-EIP aggregates hide multiple callers (`Matrix_Copy3x4`'s 131 call sites all watch as one EIP).
- Measure transfer functions instead of guessing sign conventions — [camera.md](camera.md) § Entry convention.

## Debug tools (carrier `debug_*` conf keys, default off — [carrier.md](carrier.md))

`debug_xr_probe` (in-process OpenXR on DXVK's device; run with `MC2VR_NO_HOST=1 ./launch.sh` — [host.md](host.md#open-in-process-openxr)), `debug_occlusion_off` (engine occlusion culling off, for A/B — [occluder_boxes.md](occluder_boxes.md)), `debug_build_census` (scene → draw-record build: threads, timing, task order, records per emitter/sub-view/type — [reverse_engineering/scene_record_build.md](reverse_engineering/scene_record_build.md)), `debug_vm_dump` (live thunk slot census → `vm_thunks.csv`; a 2026-10-03 copy at `reverse_engineering/data/vm_thunks_runtime.csv`), `debug_watch` (hardware watchpoints, `addr:0x…`), `debug_camtable_probe` (camera transfer-function probe — [camera.md](camera.md)), `debug_eye_dump_frames` (per-eye BMPs). `debug_gui_census` (draw-time view of the GUI records — [hud.md](hud.md)). Removed after their tracks closed (git history): `debug_terrain_cull_probe`, `debug_zstate_probe`, the E2/E2b inject_probe module, `debug_stub_trace` ([reverse_engineering/vm_stub_callbacks.md](reverse_engineering/vm_stub_callbacks.md)), `debug_rt_probe` ([render_resolution.md](render_resolution.md)), `debug_hud_probe` (minimap widget fields and per-caller `Gui_DrawTexturedRect` rects, found the border's sizing — [hud.md](hud.md)), `debug_msaa_census` ([msaa.md](msaa.md)), the SecuROM pre-probes.
