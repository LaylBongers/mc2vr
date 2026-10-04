# S4 Handover — HMD Presentation (OpenVR Compositor + Pose Feedback)

Self-contained brief for the agent picking up S4. S2 (per-eye injection) and
S2c (second draw pass) are COMPLETE and live-verified — this brief assumes
nothing from those milestones except what is stated here; per-address facts
live in Ghidra plates, milestone history in git (`git log --follow --
docs/s2c_handover.md`) and the S2c summary in `docs/stereo_design.md` §S2.

## Mission

Put the game in the headset. Concretely:

1. **Compositor in**: each frame the stereo pair (LEFT = pass 1, RIGHT =
   pass 2) goes to SteamVR via OpenVR, using the i386 OpenVR bridge already
   installed in this prefix.
2. **Pose out**: the HMD pose replaces the static ±IPD/2 camera offset —
   head position/orientation feeds the S2 camera channel so the game camera
   tracks the head.
3. **Monitor**: keeps showing the pinned LEFT image (already working; the
   game's own Present path is untouched).

The eventual consumer design (stereo_design.md §S4): Present VmtHook as the
compositor entry; slot-5 `PostUpdateHook` (claim PROVEN 1:1 with frames) as
the per-frame pose sample point; UI/2D rendered once and composited over
both eyes compositor-side. **OpenVR, NOT OpenXR** (do not re-investigate —
settled 2026-10-04; Valve's OpenXR driver has no 32-bit+DX9, wineopenxr is
registered but dead in this prefix).

## Read first (in order)

1. `AGENTS.md` (project root) — rules, iteration loop, doc conventions.
2. `docs/stereo_design.md` — architecture; §S4 for the OpenVR facts (DLL
   layout in the prefix, interop questions); §Status is current.
3. `docs/launcher_plan.md` — mechanism rules (do-not-re-litigate list),
   hook inventory, build/test commands.
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
zero failures, zero measurable cost. When S4's compositor takes over as the
real consumer, revisit whether the pin stays (the monitor path should keep
working regardless).

### Conf keys

Documented in `conf/mc2vr.conf` (read at DLL attach). Stereo pipeline keys: `view_row_rewrite=stereo`,
`view_ipd`, `view_asym_x/y`, `frame_replay`, `eye_pass`, `eye_rt`, `eye_monitor_pin`; diagnostics are
all `debug_*` and default off. The DEPLOYED conf at `<GAME_DIR>/mc2vr/` is never overwritten by
`launch.sh` — edit it there manually. Analysis tools: `tools/analyze_dumps.py` (parses view/S2c/eye
evidence), `tools/eye_pair_fixture.py`.

## S4 engineering list

1. **OpenVR bootstrap**: LoadLibrary the i386 bridge — candidates:
   `C:\vrclient\vrclient.dll` (i386, SteamVR-for-Proton runtime) and
   `syswow64\openvr_api_dxvk.dll` (DXVK-interop OpenVR API — the intended
   D3D9/DXVK texture-sharing path for 32-bit apps; no plain `openvr_api.dll`
   exists in the prefix). `openvrpaths.vrpath` (steamuser
   AppData/Local/openvr) points the runtime → `C:\vrclient\`. Open
   questions (plated in stereo_design.md §S4): which DLL the carrier should
   load, and `IVRCompositor::Submit` with `IDirect3DTexture9` under
   Proton+DXVK. SteamVR must be running (user-side setup).
2. **Per-eye LDR capture**: carrier D3D9 textures (DEFAULT pool, shared
   handles if the interop needs them), filled at the pass boundaries from
   the backbuffer (see the design observation above), or submit the fp16 RTs
   if the interop supports them (unlikely to look right — they are
   pre-tonemap; verify on the monitor first).
3. **Pose feedback**: slot-5 `PostUpdateHook` (RenderShell vtable claim
   PROVEN 1:1 with frames; re-claim on device-lost if counts stop — see
   launcher_plan hook table) samples
   `IVRSystem::GetDeviceToAbsoluteTrackingPose`, marshals to the render
   thread, and feeds `view_rewrite`: replace the static ±right·IPD/2 with
   the pose-derived per-eye offset; head rotation needs a game-camera path
   (bigger — the current channel is a translation-only pan; start with
   position + yaw, validate against the existing unit tests/log counters).
   The asym-projection channel (`view_asym_x/y`) exists for per-eye
   projection; sign convention must be validated against the HMD runtime.
4. **Submit point**: Present VmtHook (slot 17, proven) or the 2→0 pass
   boundary; pacing/timewarp considerations are S4's to design (see
   stereo_design.md §S4 and main_game_loop.md slot-4 `EndOfFrameHook`).
5. **HUD/2D**: `g_RenderQueue2` consumption timing vs Present is UNKNOWN —
   add counters when S4 starts (open question since M3). If the HUD is drawn
   per-pass into the composite, per-eye LDR captures already include it; if
   it lands between passes it may appear in only one eye — measure first.

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

- Build: `cmake -B build/win32 -DCMAKE_TOOLCHAIN_FILE=cmake/i686-w64-mingw32.cmake && cmake --build build/win32`.
- Chain selftest (needs unsandboxed terminal — wineserver uses Unix sockets,
  and `/tmp/opencode` must exist first): `tools/selftest/run.sh` — run after
  every carrier change.
- Live: the human runs `./launch.sh` into GAMEPLAY and reports; audit
  `<GAME_DIR>/mc2vr/mc2vr_*.log` (path from `launch.conf`).
  `tools/analyze_dumps.py <log>` parses view/S2c/eye evidence.
- `launch.sh` deploys fresh binaries (`cp -u`) but NEVER overwrites the
  deployed conf (`cp -n`) — conf changes there are manual.

## Open questions the new agent inherits

- `g_RenderQueue2` (2D/overlay) consumption timing vs Present — needed for the compositor's HUD
  handling; add counters when S4 starts.
- Shaders without `viewContextData`, shadow-map basis, PS-side mono camera data: see
  `stereo_design.md` § S2 remaining work and § Open questions.
- `view_asym_x/y` sign convention vs the HMD runtime.
