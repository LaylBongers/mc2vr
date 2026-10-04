# S4 Handover — HMD Presentation (OpenXR Host Process + Pose/Event Feedback)

Self-contained brief for the agent picking up S4. S2 (per-eye injection) and
S2c (second draw pass) are COMPLETE and live-verified — this brief assumes
nothing from those milestones except what is stated here; per-address facts
live in Ghidra plates, milestone history in git (`git log --follow --
docs/s2c_handover.md`) and the S2c summary in `docs/stereo_design.md` §S2.

## Mission

Put the game in the headset. The game process (32-bit, D3D9 via DXVK) cannot
talk to a VR runtime itself, so a **separate 64-bit OpenXR host executable**
(`mc2vr_host.exe`, same Wine prefix, D3D11 via DXVK) owns the OpenXR session,
presentation and event pump. Concretely:

1. **Images out**: each frame the carrier copies the stereo pair (LEFT =
   pass 1, RIGHT = pass 2) into D3D9 textures created with **shared handles**;
   the host opens them in D3D11 and submits them as an OpenXR projection layer.
2. **Pose/events back**: the host streams HMD pose, per-eye FOV, IPD, session
   state and (later) controller/action state to the carrier over IPC. The HMD
   pose replaces the static ±IPD/2 camera offset in the S2 camera channel.
3. **Monitor**: keeps showing the pinned LEFT image (already working; the
   game's own Present path is untouched).

Decisions (settled 2026-10-04, do not re-litigate): OpenVR is dropped. OpenXR
inside the 32-bit game process is impossible (Valve's OpenXR driver has no
32-bit+DX9 support), but a 64-bit D3D11 host is the supported combination.
Per-process roles: carrier = capture + inject + IPC client; host = OpenXR +
D3D11 + IPC server. Design, IPC contract and risks: `docs/stereo_design.md` §S4.

## Read first (in order)

1. `AGENTS.md` (project root) — rules, iteration loop, doc conventions.
2. `docs/stereo_design.md` — architecture; §S4 for the host/IPC/shared-handle
   design and open risks; §Status is current.
3. `docs/launcher_plan.md` — mechanism rules (do-not-re-litigate list),
   hook inventory, build/test commands, host process lifecycle.
4. `docs/render_path.md` — frame chain and threading (esp. the per-frame
   GPU-sync event-query spin in `LtiRenderer_BeginSubmit`).
5. `docs/ghidra-reva.md` — how to use the ReVa MCP tools correctly.

## Next session — start here

1. S4-0 through S4-3 are DONE — S4-3 is LIVE-VERIFIED (2026-10-04, gameplay
   run: the stereo pair visible in the headset, user-confirmed; audit in the
   S4-3 status entry — steady `submit: window fresh=600 reused=1802
   pattern=0`, zero failures both sides). Next: **S4-4 pose feedback** — the
   projection layer currently submits the RUNTIME views' pose+FOV while the
   carrier still renders the static ±IPD/2 pan; swapping in the
   carrier-rendered pose is S4-4's core (slot-5 `PostUpdateHook` +
   `view_rewrite`/`view_asym` channels, see the S4-4 entry below).
2. The S4-2 probe result (do not re-litigate): DXVK D3D9 legacy `pSharedHandle`
   textures OPEN in DXVK D3D11 (`OpenSharedResource`) across processes in
   every combination tested (D3D9Ex/plain device × A8R8G8B8/X8R8G8B8; probe:
   `tools/probe/run_shared_handle.sh`, matrix all-PASS 2026-10-04, and later
   upgraded to the full live path — RT-usage shared texture + StretchRect
   from a swapchain backbuffer + Present + event-query sync — also all-PASS).
   A8R8G8B8→DXGI 87 (B8G8R8A8_UNORM), X8R8G8B8→88 (B8G8R8X8_UNORM, alpha reads
   0xFF). Cross-process GPU sync needs ONLY a producer-side event-query flush
   (no fence on legacy handles): **`GetData` must pass `D3DGETDATA_FLUSH`** —
   without the flag DXVK never submits the command buffer and the copy never
   lands (lived through in this probe: all-zero reads + 10s query timeouts
   until the flag was added). This gotcha applies to any future carrier-side
   GPU-sync spin.
3. S4-3 is committed (`86abf73` "Implement stereo present to HMD."); the
   working tree is clean. The deployed conf has `eye_share=on` — LEAVE it on
   (it is the S4 steady state; `launch.sh` never overwrites that file). The
   in-tree conf stays `eye_share=off` (default for host-less runs).
4. User-visible state after S4-3 (expected, not bugs): the HMD shows two
   letterboxed 16:9 flat images that do NOT respond to your head. Diagnosis
   on record (2026-10-04, settled — do not re-derive): (a) head-locked feel
   because the projection layer submits the runtime's pose while the carrier
   renders the static ±IPD/2 pan — that swap is S4-4's core; (b) letterbox
   because the game's 16:9 projection fills only part of the ~0.9-aspect eye
   image — the projection-FOV/aspect match is a carve-out AFTER S4-4 (see
   Open questions: "Projection FOV/aspect fill"); (c) 30 Hz game vs ~120 Hz
   host cadence — pacing/timewarp is S4-5.

Host source map (`src/host/`): `main.cpp` (args `--mock --frames N --xr-debug`, log setup),
`xr_session.cpp` (instance/session/swapchains/event pump/frame loop, Wine VR registry fixups),
`mock.cpp` (no-runtime mode + S4-3 blit smoke test), `d3d.cpp` (D3D11 device on runtime LUID),
`eyes.cpp` (test pattern), `shared_eyes.cpp` (open cache, mirror window, `latest()` seam),
`submit.cpp` (S4-3 swapchain blit), `log.cpp`, `pose.hpp`. Host is statically linked.
Host frame rate observed ~120 Hz in the live run; submit stats: fresh counts per-EYE blits
of new carrier frames, reused covers the rest (log line says both units explicitly).

## Current state (2026-10-04 end of day, all live-verified unless noted)

### The stereo pair exists every frame

- `frame_replay=on` (S2c-1): the frame is submitted twice through
  `PgPrimitive_SubmitToGPU` (InlineHook at entry `0x00855690`,
  `src/carrier/debug/stream_capture.cpp`). Pass 1 = LEFT eye, pass 2 = RIGHT eye
  (`eye_pass=on`; deterministic per-frame eye selection via
  `view::set_pass_eye`, replacing the `view_stereo_hold` A/B timer).
- `eye_rt=on` (S2c-2): pass-2 `SetRenderTarget(0, mainRT)` and pass-2
  StretchRect/UpdateSurface sources pointing at the main RT are redirected
  to a carrier-created backbuffer-sized **eye RT** (device-level
  substitution only; game caches untouched). `src/carrier/eye_replay.cpp`.
- **Parallax PROVEN** (run 3): 5 gameplay BMP pairs measure a consistent
  **−7px horizontal parallax** (SAD 2.16 at best shift vs 3.28 at shift-0;
  sign geometrically correct for a right-camera pan). Both passes render
  fully every frame with distinct view constants (~190k rewritten VP rows
  per 10s window in gameplay).
- `delta=0` in every stream window since S2c-0 (walk table == execution);
  replay 1:1 (Present = 2× frames, BeginScene/EndScene = 1× per pass).

### The per-frame data assets (S4 consumes these)

| Asset | Content | Format |
|---|---|---|
| game main RT | pass-1 LEFT, **pre-composite fp16 HDR** | A16B16G16R16F, 2560×1440 |
| carrier eye RT | pass-2 RIGHT, pre-composite fp16 HDR | same (created from main-RT desc) |
| backbuffer @ 1→2 boundary | pass-1 final LEFT composite (tonemapped LDR, what the monitor shows) | X8R8G8B8 2560×1440 |
| backbuffer @ 2→0 boundary | pass-2 final RIGHT composite (before the pin restore overwrites it) | same |
| carrier snapshot RT | monitor-pin working copy of pass-1's final composite | same, backbuffer-desc |

**Design observation for S4**: the game's own composite (a single DRAW
into RT0=backbuffer — counter-proven run 3; no pass-2 StretchRect ever
targets the backbuffer; UpdateSurface/UpdateTexture never fire in pass 2,
those hooks are diagnostic/insurance) already produces the tonemapped LDR
per-eye finals on the backbuffer at the pass boundaries. The monitor pin's
save/restore pattern (proven free: perf identical, run 4) shows exactly how
to capture them: at the 2→0 boundary the backbuffer holds the RIGHT final
*before* `pin restore` overwrites it — a carrier blit at that instant (or
re-using the pin's own restore order) yields the monitor-exact LDR pair
without reimplementing the tonemap. Whether the composite includes the
HUD/2D is unknown (see open questions) — it did include pass-1's final
image page-exact (the pin's restore of that snapshot is what makes the
monitor stable).

### Frame chain facts (all counter-verified)

- Per frame: pass-1 submit → pass-2 submit; each submit STARTS with
  "Present prev" (presents the backbuffer from the previous pass) →
  **2 Presents per frame**, each pass ends with its composite draw into the
  backbuffer. ~30 fps (each pass ~16.6 ms; 2 passes > 60 Hz vsync budget —
  S4 pacing owns the fix; users notice the 30 Hz once the image is stable).
- GPU sync: every submit begins by waiting on all prior GPU work (event-
  query spin in `LtiRenderer_BeginSubmit`).
- Swapchain: 2560×1440, fmt 22 (X8R8G8B8), fullscreen, **SwapEffect=DISCARD
  (backbuffer content after Present is UNDEFINED — never suppress backbuffer
  writes; live-observed stale-page artifact)**, count=1.
- Final composite path differs by game state (counter-proven 2026-10-04):
  GAMEPLAY ends each pass with a single DRAW into RT0=backbuffer
  (bbRtSets=1/pass-2 frame, no pass-2 StretchRect ever targets the
  backbuffer, UpdateSurface/UpdateTexture never fire); MENUS/LOADING
  instead use a mainRT→backbuffer StretchRect as the final copy. Pass-
  boundary capture (below) works for both since it keys on boundaries, not
  the mechanism.
- Render threading: all D3D device use on the main/render thread; hook
  handlers run there.

### Monitor pin (works, keep it)

`eye_monitor_pin=on`: snapshot backbuffer→snapshot RT at the 1→2 boundary,
restore after pass 2 (2→0) via `device::blit_surfaces` (original-method
StretchRect trampoline, bypasses the hook chain). Both per-frame Presents
show the same LEFT image. pinSaves ≈ pinRestores ≈ bbRtSets ≈ 1/pass-2 frame,
zero failures, zero measurable cost. When the host takes over as the
real consumer, revisit whether the pin stays (the monitor path should keep
working regardless).

### Conf keys

Documented in `conf/mc2vr.conf` (read at DLL attach). Stereo pipeline keys: `view_row_rewrite=stereo`,
`view_ipd`, `view_asym_x/y`, `frame_replay`, `eye_pass`, `eye_rt`, `eye_monitor_pin`, `eye_share` (S4-2;
in-tree default off, but the DEPLOYED conf at `<GAME_DIR>/mc2vr/` has it ON since the S4-3 live run —
the S4 steady state, leave it on); diagnostics are
all `debug_*` and default off. The DEPLOYED conf is never overwritten by
`launch.sh` — edit it there manually. Analysis tools: `tools/analyze_dumps.py` (parses view/S2c/eye
evidence), `tools/eye_pair_fixture.py`. S4-1 env vars (not conf keys): `MC2VR_NO_HOST` (launcher skips
the host), `MC2VR_IPC_NAME` (rename the IPC section; default `mc2vr_ipc_v1`).

## S4 engineering list

Milestones are ordered so each is testable alone; the host is developed and
selftested first without the game.

- **S4-0 Host skeleton** (`src/host/`, win64 mingw toolchain — new
  `cmake/x86_64-w64-mingw32.cmake` + `build/win64`): OpenXR loader (vendored
  Khronos), D3D11 device (`XR_KHR_D3D11_enable`), session + reference space,
  pose polling, `--mock` mode with no runtime (synthetic pose, test-pattern
  eye images) so IPC and the selftest work without an HMD. First check: does
  the prefix's 64-bit OpenXR runtime (`ActiveRuntime` → `C:\openxr\wineopenxr64.json`,
  already registered) create a session with a D3D11 binding under SteamVR?
  **Status (2026-10-04)**: COMPLETE and user-verified in the headset (red left / blue right pulsing pattern; SYNCHRONIZED→VISIBLE→FOCUSED, ~120 fps frame loop). `src/host/` builds to
  `build/win64/bin/mc2vr_host.exe` (`cmake -B build/win64 -DCMAKE_TOOLCHAIN_FILE=cmake/x86_64-w64-mingw32.cmake`;
  loader built from vendored SDK 1.1.54 in `vendor/openxr-sdk/`). `--mock` verified under plain Wine.
  Real mode verified under `proton run` with SteamVR up: instance (`SteamVR/OpenXR 2.17.10`,
  `XR_KHR_D3D11_enable` present), D3D11 session on the runtime's LUID adapter (DXVK), LOCAL space,
  2 swapchains **2016x2240**, 3 images each, formats offered: sRGB-only for 8-bit
  (29 = R8G8B8A8_SRGB first, 91 = B8G8R8A8_SRGB; no plain UNORM — matters for S4-3), real head pose,
  IPD 0.0640, asym FOV (L = -0.994/0.812/0.953/-0.954 rad). Clean IDLE→READY→SYNCHRONIZED→EXITING.
  Gotchas (all handled in `xr_session.cpp::init_wine_vr_registry`):
  1. Must run through `proton run` (plain `wine` gets wined3d: "Given ID3D11Device doesn't support
     IDXGIVkInteropDevice").
  2. wineopenxr negotiation fails (-6, runtime "lacks" every extension) unless `HKCU\Software\Wine\VR`
     exists, has `wineopenxr_init_registry()` run, and a DWORD `state`=1 (normally published by
     vrclient_x64 when an OpenVR app starts; state=2 fails). The host creates/fills these itself.
  3. `--xr-debug` captures loader tracing to `mc2vr_host_xrloader.log`; stdout is lost under
     `proton run`, read `mc2vr_host.log`.
  The earlier "stuck in SYNCHRONIZED" was SteamVR itself having crashed; after restarting it the session went VISIBLE in ~3 s and FOCUSED ~5 s later. If it recurs, check SteamVR first.
- **S4-1 IPC + lifecycle**: shared-memory block + event rings (contract in
  stereo_design.md §S4). Launcher spawns the host before the game and waits
  for its ready line; carrier connects in stage 1 (non-fatal: no host ⇒ the
  game runs unmodified-mono-plus-stereo-on-monitor as today). Selftest
  extended: host `--mock` ↔ stand-in carrier round trip.
  **Status (2026-10-04)**: COMPLETE — selftest-verified AND live-verified
  (`./launch.sh`, SteamVR up, gameplay run). Live evidence: launcher spawned
  the host (ready in ~260 ms), carrier `ipc: connected`, state transitions
  mirrored host-side 1:1 (SYNCHRONIZED/FOCUSED/SYNCHRONIZED), host shut down
  on carrier exit after 8473 frames (death watch works both directions).
  Fix from the live audit: a shadowed local `poseOk` in `xr_session.cpp`
  left the IPC tracked flag always false — the host's own log said `valid=1`
  while the carrier never saw a tracked pose; fixed + `-Wshadow` on the host
  target. Protocol v1:
  `src/common/mc2vr_ipc.h` (fixed-width, arch-neutral; seqlocked
  `Mc2IpcState` = pose/FOV/IPD/session/recenter; SPSC `Mc2IpcMsg` rings —
  events host→carrier, commands carrier→host incl. the S4-2 `FRAME_READY`
  shape; `hostExiting` flag + carrier-pid death watch both ways). Host:
  `src/host/ipc.cpp` creates the section (refuses if one exists — stale-host
  collision guard) before session setup; mock + real paths publish per frame
  and push session-state/recenter events; `Shutdown` or carrier death exits
  cleanly (`mark_exiting` before return). Carrier: `src/carrier/ipc.cpp`
  connects after the build-lock gate (an idle carrier never registers),
  runs a leaked 250ms monitor thread (session transitions, first tracked pose,
  host death) — the render thread stays untouched until S4-4.
  Launcher: spawns `mc2vr_host.exe` (deploy dir) before the game, waits for
  the `mc2vr_host: ready` log line (30s, non-fatal: early exit logged with
  rc, missing exe logged, `MC2VR_NO_HOST` skips) — `launch.sh` deploys the
  host (auto-builds win64 if missing). Env `MC2VR_IPC_NAME` renames the
  section (selftest isolation). Gotcha fixed en route: the seqlock read
  helper originally memcpy'd the payload into the wrong struct offset —
  every field read shifted; the probe caught it.
- **S4-2 Shared-handle image path**: carrier creates DEFAULT-pool shared
  textures (ring of N per eye, LDR X8R8G8B8/A8R8G8B8 only — the fp16 RTs are
  pre-tonemap and not shareable-friendly), fills them at the pass boundaries
  from the backbuffer (see design observation above), publishes
  `{frameId, slot, eye, handle, size, format}` to the host; host opens them
  (`OpenSharedResource`) and mirrors to a desktop window first (verifies the
  path before touching OpenXR submission). **Unproven under DXVK** — whether
  DXVK's D3D9 `pSharedHandle` handles open in DXVK's D3D11 across processes
  is the key S4 risk; fallbacks are listed in stereo_design.md §S4. Decide
  with a 20-line two-process probe before building anything else on it.
  **Status (2026-10-04)**: **COMPLETE — LIVE-VERIFIED** (gameplay run: the
  mirror window showed the live stereo pair, two visibly offset cameras; the
  carrier `share window:` counters and host `seyes: window stats` both ran
  ~30 Hz with zero failures). Mechanism proven by the probe (`tools/probe/
  run_shared_handle.sh`, later upgraded to the full live path — RT-usage
  shared texture + StretchRect from a swapchain backbuffer + Present +
  event-query sync; matrix all-PASS; gotchas live in the probe sources and
  "Next session" item 2). En route the live runs shook out three bugs — all
  fixed, all lessons recorded: (1) raw-vtable texture slot GetSurfaceLevel=18
  (not 12), (2) query slots Issue=6/GetData=7 (not reversed — reversed slots
  crashed the game writing through pointer 0x1), (3) the host mirror never
  CopyResource'd before Map (black panes). Implemented components:
  - Carrier: `src/carrier/eye_share.cpp` (conf `eye_share=off|on`, needs
    `frame_replay=on`; default OFF — flip it in the DEPLOYED conf at
    `<GAME_DIR>/mc2vr/mc2vr.conf`, which launch.sh never overwrites). At the
    pass boundaries `eye_replay::set_pass` calls
    `share::on_pass_boundary`: 1→2 blits the backbuffer (LEFT final) into
    ring slot N, 2→0 blits the backbuffer (RIGHT final, BEFORE the monitor-pin
    restore) into the same slot N, each followed by a bounded (8 ms) event-query
    GPU sync (`D3DGETDATA_FLUSH`!) and a `FRAME_READY` push; the slot advances
    after both eyes publish. Ring = 4 slots/eye of RENDERTARGET-usage textures
    (StretchRect requires RT surfaces) created with `pSharedHandle` via
    device-slot 23. `CONFIG` (w/h/fmt) is sent once per ring creation. No host
    ⇒ module inert (no allocation); Reset drops and re-creates the ring
    lazily. Counters in the 10s `share window:` line (capturesL/R, published,
    ringFull, syncTimeouts, noHostSkips).
  - IPC: carrier `ipc::send_config`/`send_frame_ready` (`src/carrier/ipc.cpp`);
    shapes unchanged from the header (`x=frameId y=handle a=slot b=eye c=w d=h`).
  - Host: `src/host/shared_eyes.cpp` opens every handle once (cache, reserved
    to 64 so Entry pointers stay stable across post-Reset handle waves), keeps
    the newest per eye, and mirrors the pair to a 1280×720 desktop window
    `mc2vr host mirror [ L | R ]` (GDI StretchDIBits from a staging read — no
    shaders/swapchain, cannot disturb the OpenXR session). Wired into
    `xr_session.cpp`'s command drain + `seyes::pump()` per host frame;
    `--mock` logs the new commands (no mirror) — the selftest probe sends
    CONFIG + FRAME_READY(handle 0) as a shape check.
  - Note for S4-3: the mirror's GDI staging-read path stays useful as a
    diagnostic; the OpenXR submission replaces it as the primary consumer of
    the SAME opened shared textures (`seyes::on_frame_ready` cache +
    `g_latest[]` are the seam to reuse).
- **S4-3 OpenXR submission**: host copies/uses the shared images as the
  swapchain content, `xrEndFrame` projection layer with the pose+FOV the
  carrier reports having rendered with (lets the runtime reproject). Host
  runs `xrWaitFrame` at HMD cadence and re-submits the newest pair while the
  game runs ~30 Hz.
  **Status (2026-10-04)**: **COMPLETE — LIVE-VERIFIED** (gameplay run:
  the stereo pair visible in the headset; user-confirmed). Live evidence
  (mc2vr_host.log / mc2vr_carrier.log audit): format 91 chosen with RTV cast
  87 as designed; `submit: window fresh=600 reused=1802 pattern=0` steady
  state (fresh = per-eye blits of new carrier frames → 30 Hz game,
  reused re-submits the newest pair up to ~120 Hz host cadence; pattern=2
  only in the very first window before the first pair arrived); carrier
  `share window:` steady 300 L/R per 10s, ringFull=0 (the S4-2 residual
  backpressure never recurred), syncTimeouts=2 total across the whole run
  (bounded 8ms event-query flush occasionally times out — benign),
  noHostSkips=0; mirror `seyes:` opened=8 (2 eyes × 4 ring slots),
  openFails=0. Residuals noted for S4-4: pose/FOV in the projection layer
  are still the RUNTIME views (see below), and the host loop runs ~120 Hz
  while the game runs ~30 Hz — pacing/timewarp quality is S4-5's
  `EndOfFrameHook` plate. Log-line unit fix post-run: `submit: window`'s
  rate line now says "eye blits ~N/s, host frames ~N Hz" (the old label
  said "frames" but counted per-eye blits — 2 per host frame). Implemented:
  - `src/host/submit.cpp` — fullscreen-triangle blit (vs/ps 4_0 compiled at
    startup via a dynamic `d3dcompiler_47.dll` load — no link-time import)
    drawing the newest shared-eye image into the acquired swapchain image,
    aspect-fit 2560x1440 → e.g. 2016x2240 (letterbox on black) via a viewport
    rect; alpha forced opaque (the X8R8G8B8 source opens as B8G8R8X8, alpha
    reads 0xFF — probe-proven). All bound state unbound after the draw.
    Failure is non-fatal: the loop falls back to the S4-0 test pattern
    (pulsing in the headset = carrier pipeline not talking / `eye_share=off`).
  - **sRGB handling** (the 29/91 issue): runtime swapchains are sRGB-typed and
    `CopyResource` is illegal across UNORM↔sRGB — so the blit passes the
    sRGB-encoded LDR finals through as RAW bytes via UNORM-CAST views on both
    ends: SRV `B8G8R8A8_UNORM` on the shared texture (castable from 87/88;
    added per-entry in `shared_eyes.cpp`) and the swapchain RTV cast to the
    plain-UNORM sibling (91→87, 29→28 — `create_swapchains` now prefers 91
    then 29; the old code preferred plain UNORM, which the runtime never
    offers). The compositor decodes the sRGB swapchain — exactly what
    display-referred content means — byte passthrough, no double gamma.
  - `xr_session.cpp frame()`: per eye, newest shared image via the new
    `seyes::latest()` accessor (reuses the S4-2 open cache + `g_latest[]`
    seam; does NOT consume the mirror's fresh flag — re-submission at HMD
    cadence is the point) is blitted into the swapchain; the projection layer
    still uses the RUNTIME views' pose+FOV (the carrier still renders the
    static ±IPD/2 pan — the carrier-reported pose lands in S4-4). ~10s
    `submit: window fresh/reused/pattern` stats line = the acceptance
    evidence.
  - Live-run checklist (DONE 2026-10-04, kept for reruns): SteamVR up,
    `eye_share=on` in the DEPLOYED conf, gameplay. In `mc2vr_host.log`
    expect: `submit: blit shaders ready`, `openxr: using swapchain format 91`,
    `submit: window` lines, and the mirror still working alongside.
- **S4-4 Pose feedback**: slot-5 `PostUpdateHook` (RenderShell vtable claim
  PROVEN 1:1 with frames; re-claim on device-lost if counts stop) reads the
  latest pose from the shared block (lock-free, never blocks the render
  thread), feeds `view_rewrite`: replace ±right·IPD/2 with the pose-derived
  per-eye offset; head rotation needs a game-camera path (bigger — the
  current channel is a translation-only pan; start with position + yaw,
  validate against existing log counters). The asym-projection channel
  (`view_asym_x/y`) takes the host-reported per-eye FOV; sign convention
  must be validated against the runtime.
- **S4-5 Events + pacing + HUD**: session-state events (focus lost ⇒ game
  stays running but host stops submitting; exit request ⇒ clean shutdown),
  recenter, pacing/timewarp inputs (slot-4 `EndOfFrameHook`, see
  `main_game_loop.md`). HUD/2D: `g_RenderQueue2` consumption timing vs
  Present is UNKNOWN — add counters. If the HUD is drawn per-pass into the
  composite, per-eye LDR captures already include it; if it lands between
  passes it may appear in only one eye — measure first.

## Hard rules (violating these wastes days — see launcher_plan.md for why)

- Plaintext `.text` only. NEVER hook anything at `0x01a48000+`, the VM entry
  stub `0x0050f660`, or any VM-stub thunk (calling thunks is fine, proven).
  The packet interpreter behind `0x0050f660` is VM'd — bracket its plaintext
  call sites (`0x004c99f9`/`0x004c99fe`), never the stub.
- No thread suspension at hook install (SafetyHook trap-based; suspension
  deadlocks on the CRT heap lock).
- The device vtable is the only sanctioned vtable patch (cloned-vtable
  VmtHook). The RenderShell slot-4/5 claim is also a sanctioned
  cloned-vtable swap (already proven).
- Hook objects leak by design; handlers must match the original convention.
- The game's upload buffers are never modified in place — rewrite on scratch
  copies returned to the driver call (see `view_rewrite.cpp`).
- Never suppress backbuffer writes under SwapEffect=DISCARD (stale driver
  page — live-proven). Never pass narrow strings to wide printf formats
  (mojibake filenames — live-proven; widen by hand).

## Build / test / iterate

- Build (game side): `cmake -B build/win32 -DCMAKE_TOOLCHAIN_FILE=cmake/i686-w64-mingw32.cmake && cmake --build build/win32`.
  Host (planned, S4-0): same pattern with `cmake/x86_64-w64-mingw32.cmake` → `build/win64/bin/mc2vr_host.exe`.
- Chain selftest (needs unsandboxed terminal — wineserver uses Unix sockets,
  and `/tmp/opencode` must exist first): `tools/selftest/run.sh` — run after
  every carrier change.
- Live: the human runs `./launch.sh` into GAMEPLAY and reports; audit
  `<GAME_DIR>/mc2vr/mc2vr_*.log` (path from `launch.conf`; the host logs to `mc2vr_host.log`).
  `tools/analyze_dumps.py <log>` parses view/S2c/eye evidence.
- `launch.sh` deploys fresh binaries (`cp -u`) but NEVER overwrites the
  deployed conf (`cp -n`) — conf changes there are manual.

## Open questions the new agent inherits

- **Projection FOV/aspect fill** (carve-out, do AFTER S4-4; discussed +
  settled 2026-10-04): the HMD currently shows letterboxed 16:9 flat images —
  beyond the missing head pose (S4-4), the game's baked 16:9 projection fills
  only part of the ~2016x2240 (~0.9 aspect, ~100°+) per-eye images. Fixing it
  means extending the GPU-boundary rewrite (`view_rewrite`, currently a rigid
  translation-only pan on the VP rows) to change the PROJECTION — per-eye
  FOV/aspect matching the host-reported FOV — so the render fills the eye
  image. Related but distinct: `view_asym_x/y` (projection CENTER shift) is
  already scoped into S4-4; the FOV/aspect change itself is not yet scoped.
  Open sub-questions: which VP rows are projection vs view (S2 mapping in
  Ghidra plates), per-technique coverage (180/194 shaders carry
  viewContextData — do all bake FOV?), and whether widened FOV breaks
  culling/LOD assumptions in the engine (near edges of view frusta are
  game-side data).
- S4-2 residual watch items (live-verified 2026-10-04, but keep an eye
  out): `ringFull` was 16 in one 10s carrier window during the S4-2 live run
  (command-ring backpressure — benign at these rates; stayed 0 across the
  whole S4-3 run), and the backbuffer MS type is now logged at ring creation.
  S4-3-run note: syncTimeouts=2 total across the whole run (bounded 8 ms
  event-query flush — benign, logged for the record).
- `g_RenderQueue2` (2D/overlay) consumption timing vs Present — needed for HUD
  handling; add counters when S4 starts.
- Shaders without `viewContextData`, shadow-map basis, PS-side mono camera data: see
  `stereo_design.md` § S2 remaining work and § Open questions.
- `view_asym_x/y` sign convention vs the OpenXR FOV convention.
