# S4 Handover — HMD Presentation (OpenXR host + pose/event feedback)

Brief for the agent picking up S4-5. S0–S2c (per-eye injection, second draw pass) and S4-0..S4-4 are
COMPLETE and live-verified; design, algebra and durable gotchas live in `docs/stereo_design.md` (§S4,
§S4-4) — this file is only status, the S4-5 task list and the live-run workflow. Per-address facts live in
Ghidra plates; milestone history in git.

## Read first

1. `AGENTS.md` — rules, iteration loop. 2. `docs/stereo_design.md` — architecture, view channel, S4 host/IPC
design + gotchas. 3. `docs/launcher_plan.md` — mechanism rules, hook inventory, build/test. 4. `docs/reverse_engineering/render_path.md`
— frame chain/threading (GPU-sync event-query spin in `LtiRenderer_BeginSubmit`). 5. `docs/reverse_engineering/ghidra-reva.md`.

## State (2026-10-05)

| Milestone | State |
|---|---|
| S4-0 host skeleton (OpenXR/D3D11 session under Proton+SteamVR) | DONE, headset-verified |
| S4-1 IPC + lifecycle (shared-mem block, rings, death watch both ways) | DONE, live-verified |
| S4-2 shared-handle image path (carrier ring → host open/mirror) | DONE, live-verified |
| S4-3 OpenXR submission (blit shared pair into swapchains) | DONE, live-verified |
| S4-4 pose feedback (full VP replacement from HMD pose, pose-id handoff) | DONE, live-verified: head-tracked 3D in the HMD; `view/hmd:` windows `split=0 decompFail=0`; host `pose ids hit=all miss=0`; `fresh~600 reused~1800` |
| S4-5 events + pacing + HUD | IN PROGRESS — pacing step 1 (vsync unlock) live-verified: 30 → ~135 Hz sustained while VR-active; session events live-verified (2026-10-06: 2× SteamVR recenter → logged + view stable; SteamVR quit → EXIT chain → game quit cleanly). Remaining: HUD/2D measurement, throttle-vs-free decision (free-run healthy) |

Deployed conf (`<GAME_DIR>/mc2vr/mc2vr.conf`, never overwritten by `launch.sh`) is the S4 steady state:
`frame_replay=on eye_pass=on eye_rt=on eye_monitor_pin=on eye_share=on view_row_rewrite=hmd`. The in-tree conf
defaults stay host-less (`eye_share=off`, `view_row_rewrite=stereo`).

## S4-5 task list

- **Session events**: IMPLEMENTED (2026-10-06, pending live verification). Drain point moved to the
  slot-5 `PostUpdateHook` (`ipc::drain_events()`, main thread, once per frame; the 250ms monitor thread no
  longer pops — SPSC ring, one consumer only). Behavior: `SESSION_STATE` transitions logged; `RECENTER`
  logged only (nothing to apply — the camera consumes live HMD poses, so a reference-space change
  propagates at the next pass-1 pose sample by construction); `MC2VR_MSG_EXIT` = one-shot `WM_CLOSE` to
  the game's root window (HWND global `0x01175274`; pump/WndProc quit path is VM-protected so the
  message is the clean-quit signal; engine pump exits → carrier pid dies → host follows via death
  watch). Host side: EXIT is pushed only for runtime-initiated ends (session EXITING/LOSS_PENDING or
  instance loss WITHOUT a prior self-initiated `xrRequestExitSession` — `State::selfExit` flag set at
  the Shutdown-cmd/carrier-death/frame-limit sites). Focus lost needs no carrier action: the host
  already gates layer submission on `shouldRender` (zero layers while SYNCHRONIZED/idle), and the
  game keeps running with the camera pass-through (`valid=0` path, live-verified 2026-10-06).
  LIVE-VERIFIED 2026-10-06: SteamVR recenter ×2 → carrier `session: recenter #N` (3 events: one
  boot-time reference change + the 2 user recenters), view stayed healthy (`split=0 decompFail=0`);
  SteamVR quit in-game → host `runtime is ending the session` → carrier `posted WM_CLOSE to the game
  window` → game exited cleanly (no FATAL/failed lines, host shutdown 9210 frames, both logs end
  clean). FPS with VR active stayed ~135 Hz (dt avg 7.2–7.8 ms, max ≤18 ms while FOCUSED), host
  ~120 Hz `miss=0`.
- **Pacing**: game ~30 Hz (two ~16.6 ms passes) vs host ~120 Hz re-submitting the newest pair with the
  rendered pose (runtime reprojects). STEP 1 IMPLEMENTED (2026-10-06, pending live verification):
  `vsync=off` conf key forces `D3DPRESENT_INTERVAL_IMMEDIATE` at CreateDevice (stage-1 InlineHook of
  the game's `Direct3DCreate9` IAT thunk `0x00a4e892` + IDirect3D9 VmtHook slot 16; Reset re-patches —
  the engine's own CreateDevice call site is VM-gated `FUN_0074c9b0`, never hooked). Present-count
  evidence first: ~600 Presents / ~300 frames per 10s = 2 blocking Presents per frame. LIVE-VERIFIED
  2026-10-06 (with and without HMD out of standby): interval=0x80000000 in effect, dt avg ~7.4 ms
  (~135 Hz gameplay, 3.2 ms per pass — the 30 Hz was pure vsync wait), host `pose ids miss=0`, carrier
  `split=0 decompFail=0 ringFull=0`, HMD standby ⇒ session SYNCHRONIZED + carrier `valid=0` pass-through
  (by design), clean shutdown. Remaining decision:
  throttle-to-HMD vs run-free (timewarp inputs via slot-4 `EndOfFrameHook`, see
  `docs/reverse_engineering/main_game_loop.md`); adaptive-framerate bypass (`g_FrameratePolicy` /
  `AdaptiveFramerate_Govern`) if the game fights the new cadence.
- **HUD/2D**: `g_RenderQueue2` consumption timing vs Present is UNKNOWN — add counters. If the HUD is drawn
  per-pass into the composite, the per-eye LDR captures already include it; if it lands between passes it may
  appear in one eye only — measure first. Fallback: separate quad layer in the host.
- **S4-4 follow-ups** (can interleave): measure `view_world_scale` (game units/metre — unverified default 1.0;
  the 0.065 IPD was never checked); engine culling still uses the GAME camera frustum (edge pop-in at wide
  FOV / head turns — check whether the game FOV is reachable); non-`viewContextData` shaders, PS-side camera
  data and texgen stay mono/lag with rotation; pixel density of 2560×1440 over a ~100°+ eye frustum.

## Live-run workflow

Agent implements/logs; the human runs `./launch.sh` into GAMEPLAY (SteamVR up) and reports; the agent audits
`<GAME_DIR>/mc2vr/mc2vr_{carrier,host,launcher}.log` (`GAME_DIR` from `launch.conf`;
`tools/analyze_dumps.py <log>` parses view/S2c/eye evidence). Healthy-run signatures: host `submit: blit shaders
ready`, `openxr: using swapchain format 91`, `submit: window fresh/reused/pattern=0`, `submit: pose ids miss=0`;
carrier `share window:` ~1330 L/R per 10 s with `ringFull=0` (≈ game fps × 10; ~300 was the
vsync-locked 30 Hz era) and `view/hmd: split=0 decompFail=0`. Game cadence with `vsync=off`:
`FrameTick: dt avg` ~7.4 ms (~135 Hz). Pulsing pattern in
the HMD = carrier pipeline not talking (`eye_share=off` or host not receiving). Build: win32 carrier
`cmake -B build/win32 -DCMAKE_TOOLCHAIN_FILE=cmake/i686-w64-mingw32.cmake`; win64 host same with
`cmake/x86_64-w64-mingw32.cmake` → `build/win64/bin/mc2vr_host.exe`; chain selftest `tools/selftest/run.sh`
(unsandboxed terminal; `mkdir -p /tmp/opencode` first) after every carrier/host change. Pure camera math: `tools/test/test_vp_camera.cpp` (native g++, one-line build in its header). `launch.sh` deploys
with `cp -u` (binaries) / `cp -n` (conf). Env: `MC2VR_NO_HOST` (skip host), `MC2VR_IPC_NAME`.

## Hard rules (see launcher_plan.md for why)

- Plaintext `.text` only. NEVER hook `0x01a48000+`, the VM stub `0x0050f660`, or any VM-stub thunk (calling
  thunks is fine). Bracket the packet interpreter's plaintext call sites (`0x004c99f9`/`0x004c99fe`).
- No thread suspension at hook install (trap-based SafetyHook; suspension deadlocks on the CRT heap lock).
- Sanctioned vtable patches only: device cloned-vtable VmtHook, RenderShell slot-4/5 claim. Hook objects leak by
  design; handlers match the original convention.
- Game upload buffers are never modified in place — rewrite scratch copies (`view_rewrite.cpp`).
- Never suppress backbuffer writes (SwapEffect=DISCARD ⇒ stale-page artifact). Never pass narrow strings to wide
  printf formats. GPU-sync `GetData` must pass `D3DGETDATA_FLUSH` (DXVK never submits otherwise).
