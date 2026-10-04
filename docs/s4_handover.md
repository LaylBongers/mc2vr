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
`view_ipd`, `view_asym_x/y`, `frame_replay`, `eye_pass`, `eye_rt`, `eye_monitor_pin`; diagnostics are
all `debug_*` and default off. The DEPLOYED conf at `<GAME_DIR>/mc2vr/` is never overwritten by
`launch.sh` — edit it there manually. Analysis tools: `tools/analyze_dumps.py` (parses view/S2c/eye
evidence), `tools/eye_pair_fixture.py`.

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
- **S4-1 IPC + lifecycle**: shared-memory block + event rings (contract in
  stereo_design.md §S4). Launcher spawns the host before the game and waits
  for its ready line; carrier connects in stage 1 (non-fatal: no host ⇒ the
  game runs unmodified-mono-plus-stereo-on-monitor as today). Selftest
  extended: host `--mock` ↔ stand-in carrier round trip.
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
- **S4-3 OpenXR submission**: host copies/uses the shared images as the
  swapchain content, `xrEndFrame` projection layer with the pose+FOV the
  carrier reports having rendered with (lets the runtime reproject). Host
  runs `xrWaitFrame` at HMD cadence and re-submits the newest pair while the
  game runs ~30 Hz.
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

- DXVK D3D9→D3D11 shared-handle interop across processes (S4-2 probe), and cross-process
  GPU synchronization for it — see stereo_design.md §S4.
- `g_RenderQueue2` (2D/overlay) consumption timing vs Present — needed for HUD
  handling; add counters when S4 starts.
- Shaders without `viewContextData`, shadow-map basis, PS-side mono camera data: see
  `stereo_design.md` § S2 remaining work and § Open questions.
- `view_asym_x/y` sign convention vs the OpenXR FOV convention.
