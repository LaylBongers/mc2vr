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
5. `docs/render_path.md` — frame chain and threading (esp. the per-frame
   GPU-sync event-query spin in `LtiRenderer_BeginSubmit`).
6. `docs/ghidra-reva.md` — how to use the ReVa MCP tools correctly.

## Current state (2026-10-04 end of day, all live-verified unless noted)

### The OpenVR bridge is live (S4-1 Stage 1 COMPLETE, run 6b in game)

`openvr=on` (deployed): at attach the carrier writes the three
`HKCU\Software\Wine\VR` values, `VR_InitInternal2(Scene)` connects clean,
then the key is deleted again — game boots are interop-free and
SteamVR-state-independent. Verified in game: Valve/Index/LHR-C69559AF,
RT 2016x2240, 120 Hz, pose valid=1. FnTable calls are THISCALL (no self).
Full history + the three data anomalies: §S4 list item 1.

### The stereo pair exists every frame

- `frame_replay=on` (S2c-1): the frame is submitted twice through
  `PgPrimitive_SubmitToGPU` (InlineHook at entry `0x00855690`,
  `src/carrier/stream_capture.cpp`). Pass 1 = LEFT eye, pass 2 = RIGHT eye
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

### Conf keys (mc2vr.conf, read at DLL attach)

`view_row_rewrite=stereo` (the S2 camera channel: D = ±right·IPD/2 on the
viewContextData VP rows; gate = RT0 backbuffer-sized, main pass only) ·
`view_ipd` (0.065) · `view_asym_x/y` (asym-projection NDC channel, default
0, sign convention unvalidated) · `frame_replay` · `eye_pass` · `eye_rt` ·
`eye_monitor_pin` · `eye_dump_frames` (0 now; N dumps N non-empty L/R BMP
pairs after `stream_dump_delay` s; fp16 decoded + Reinhard-tonemapped;
analysis: `tools/analyze_dumps.py`, fixture: `tools/eye_pair_fixture.py`) ·
`stream_capture` (+ `stream_dump_frames`/`stream_dump_delay`) · `openvr` ·
  `openvr_submit` (**off in code** — S4-2 capture+Submit, requires
  `openvr=on`; see §S4 list item 2 for the error-decision table) ·
`openvr_init_registry` (**off** — live-proven poison, run 5; the err-105 fix
is now the carrier-time registry window instead) ·
`stub_trace` · `vm_dump`. Defaults off in code; conf template `conf/mc2vr.conf`. The
DEPLOYED conf at `<GAME_DIR>/mc2vr/` is never overwritten by `launch.sh` —
edit it there manually.

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
   **RESOLVED 2026-10-04 (S4-1)**: carrier module `src/carrier/openvr_bridge.cpp`
   (conf `openvr=on`, deployed) — loads `openvr_api_dxvk.dll` (the only
   flat-OpenVR-API module in the prefix; both vrclient copies export only the
   raw VRClientCoreFactory). FnTable C bindings pinned to `IVRSystem_022` /
   `vendor/openvr/openvr_1.16.8.h` — the served IVRSystem_026 layout does NOT
   match the public SDK 2.15 header (slot-28 string query crashed the probe);
   the pinned-version method is verified (IVRCompositor_022 query worked 1:1).
   **THE KEY FIX for err 105 (SUPERSEDED 2026-10-04 evening — see run-5
   analysis below)**: old theory said in-process `vrclient_init_registry`
   populates an in-process cache. RE + experiments proved the fix is three
   persistent `HKCU\Software\Wine\VR` values (PROTON_VR_RUNTIME, state=1,
   openvr_vulkan_instance_extensions=""), which launch.sh now asserts via
   reg.exe pre-game; the in-game init_registry call is gated OFF
   (`openvr_init_registry=off`) because its heavy path poisons the process
   (FreeLibraryAndExitThread under live sessions — run 5). Probe
   (`MC2VR_PROBE_SKIP_INITREG=1` + the reg recipe) connects cleanly with zero
   asserts. **Init order matters**: VR_InitInternal2 first, version probes
   after (pre-init probes read 0 regardless). **Bootstrap runs on a dedicated
   thread** — early OpenVR calls block during vrserver standby transitions.
   **LANDMINE (white-screen boot hang — LIVE-CONFIRMED in run 6a)**: with
   `openvr_vulkan_instance_extensions` present at process start, DXVK's
   d3d9 arms its boot-time OpenVR interop at device creation and blocks
   reaching SteamVR mid-transition — the game never reached the main loop.
   The carrier therefore writes the values only inside its attach→init2
   window and DELETES the key again afterwards (run 6b design); nothing
   else may write that key (see run 6a/6b analysis below).
   Diagnostics: `tools/openvr_probe/openvr_probe.cpp` (standalone, `proton
   run` in any prefix, no game; calls init_registry itself; 022-based smoke
   queries incl. a pose sample). NOTE /tmp is wiped between terminal calls.
   **Run 5 (2026-10-04, first in-game Stage-1 attempt): HUNG — FULLY ROOT-CAUSED
   same day (probe experiment matrix + Ghidra RE of the builtin vrclient.dll /
   i386-unix vrclient.so; run-5 analysis below SUPERSEDES the first-pass
   teardown-race theory).** THREE independent root causes were unmasked:

   **(a) err 105 = missing `HKCU\Software\Wine\VR` key (NOT HKLM, NOT
   in-process).** RE of Proton's builtin vrclient.dll `load_vrclient`:
   `RegOpenKeyExA(HKCU=0x80000001, "Software\Wine\VR")` → absent → "Could
   not create key, status 0x2" → vrclient unloads → err 105. The function
   then reads the `state` DWORD (0 → it BLOCKS on RegNotifyChangeKeyValue
   waiting for the setup to signal ready; missing → abort) and
   `openvr_vulkan_instance_extensions`. So the fix is exactly three registry
   values (probe-proven clean connect, zero asserts, hmd present=1 PRE-init,
   NO init_registry call anywhere):
     - `PROTON_VR_RUNTIME` REG_SZ = <SteamVR path> (proton sets the env var
       too; the registry value is what vrclient reads),
     - `state` REG_DWORD = 1,
     - `openvr_vulkan_instance_extensions` REG_SZ = "" (the compositor
       reports EMPTY required extensions — semantically correct).
   Wine's registry-FILE flushing in this prefix is unreliable (observed
   writes vanishing between wineserver generations) — launch.sh now
   RE-ASSERTS all three via reg.exe on every `openvr=on` launch (no
   vrclient involvement, deterministic, idempotent). The old in-game
   `vrclient_init_registry` call stays gated OFF
   (`openvr_init_registry=off`) — see (b).

   **(b) `vrclient_init_registry`'s heavy path is live-proven poison.** RE of
   the PE export: it creates the HKCU key; if ALREADY present → "Already
   initialized, returning" (no-op light path); if absent → writes the value,
   load_vrclient(), then spawns `initialize_vr_data` on a thread. That thread
   runs the unix-side VR-info init (Background-type vrclient session,
   compositor connect, Vulkan device-extension enumeration — the
   VK_EXT_debug_utils warnings), then **`FreeLibraryAndExitThread`s the
   native vrclient** — any session live at that moment runs on freed memory.
   In run 5 that thread raced our Scene init ("VR_Init 2 times" warning,
   "init shared state ... twice" asserts, "Destroying a held mutex"), and
   the render thread froze with the bootstrap thread (the freed-library
   state is reachable from DXVK's VR-coupled paths). In a throwaway process
   the heavy path also fails to leave usable persistent state (the
   setup-tool + immediate-probe experiment still err-105'd), so there is NO
   safe way to call it in-process — the reg.exe recipe replaces it entirely.

   **(c) THE ABI BUG (biggest one — explains ALL prior "bad session"
   folklore).** The FnTable entries Proton serves are **THISCALL thunks**
   (built in `create_winIVRSystem_IVRSystem_022_FnTable`: `mov ecx, obj;
   mov edx, wrapper; jmp edx`) — they load the object into ECX THEMSELVES; the
   first STACK argument is the method's first parameter, NOT self. Our calls
   passed an explicit self, shifting every argument one slot:
     - run 5 "device 0 is not the HMD": GetTrackedDeviceClass read the table
       POINTER as the device index (always garbage) — the session was never
       dead; the class-check abort we added based on that reading was
       fighting a phantom.
     - The run-5 string query then passed a NULL buffer into the server →
       bootstrap blocked forever (and the render thread's freeze coupled
       through the DXVK path).
     - The probe's `2240x0` RT size: real HEIGHT written into *w, real width
       scribbled over the table object; *h never written.
     - GetProjectionRaw with a shifted arg (eye=pointer, pfLeft=NULL)
       crashed the probe AND watchdog-aborted vrserver ("UpdateControllerRoles")
       — every "slot-28 crash"/"026 layout mismatch" data point in the old
       notes is this bug, not a layout problem. The served 022 FnTable layout
   exactly matches the pinned 1.16.8 header (verified against the builtin's
   own builder function).
   FIXED in carrier + probe (entries called WITHOUT self). The probe now
   settles 2 s post-connect (a Scene connect WAKES a standby HMD; queries
   fired mid-transition observe a half-awake device).
   **S4-2 open question — DXVK boot-time interop**: `IVRCompositor::Submit`
   with `IDirect3DTexture9` under Proton+DXVK needs DXVK's VR interop active,
   which is initialized at DEVICE CREATION (game boot, before the carrier
   attaches) and currently deliberately OFF (scrubbed registry). Design
   options when S4-2 starts: (a) find the value content that makes DXVK's
   boot init non-blocking and correct, (b) submit via a path that doesn't
   need boot-time DXVK interop, (c) accept the interop but solve the boot
   hang (the hang was Scene-init blocking; a Background-type DXVK init might
   not block — unknown). The compositor reported its required instance
   extensions as EMPTY via IVRCompositor_022::GetVulkanInstanceExtensionsRequired.

   **RUN 6a (2026-10-04 evening): game did not start — DXVK BOOT-INTEROP
   LANDMINE LIVE-CONFIRMED.** With the three values present in the registry
   at process start (written pre-game by launch.sh at the time) and SteamVR
   mid-flap (its auto-start/no-foreground-app loop), the game hung BEFORE
   the main loop — launcher: "boot marker never ticked twice (rc=5)",
   carrier never attached. Same binary that booted clean in run 5 when the
   key was absent; the only delta was the registry values. DXVK's d3d9 arms
   its OpenVR interop at DEVICE CREATION and blocks reaching SteamVR
   mid-transition.
   **FIX SHIPPED (run 6b design): the two consumers are now decoupled by
   TIMING.** DXVK reads the key only at boot (before the carrier exists);
   vrclient reads it only at VR_InitInternal2 time (post-attach). The
   carrier now does the whole dance itself (`ensure_vr_registry` in
   openvr_bridge.cpp): write the three values at attach → init2 →
   **delete the key again** (`cleanup_vr_registry`, called on success AND
   on the give-up path — vrclient never re-reads, `_vrclient_loaded` guards
   the load). Every game boot therefore starts interop-free and is
   INDEPENDENT of SteamVR state; the launch.sh pre-game reg writes are
   removed. Registry state now: key deleted, verified.
   **S4-2 implication (firmed up)**: DXVK's boot-time interop stays OFF by
   design. When S4-2 picks the texture-sharing path it must either use a
   runtime-interop path (run 5 proved runtime activation exists via the
   in-process unix init — but that path is the FreeLibraryAndExitThread
   poison, so more RE needed) or deliberately re-arm the boot value behind a
   SteamVR-stability gate. Do NOT write
   openvr_vulkan_instance_extensions into the registry outside the
   carrier's attach→init2 window.
   **SteamVR fragility observed 2026-10-04**: a NULL out-pointer reaching the
   server (the ABI bug) watchdog-aborts vrserver (~10 s, "UpdateControllerRoles")
   and can leave vrcompositor aborting at startup — the client side must
   never send malformed IPC (the ABI fix is therefore a SAFETY requirement,
   not just correctness). Steam's SteamVR auto-start (HMD awake) plus no
   foreground app self-quits vrserver after ~20 s and restarts it — a benign
   start/stop loop that ends the moment a Scene app (the game) connects; the
   broken wineopenxr OpenXR registration in the prefix
   (C:\openxr\wineopenxr64.json missing) is noise along the way and
   deliberately left alone (OpenXR settled against).
   **RUN 6b RESULT (2026-10-04, gameplay reached): S4-1 STAGE 1 COMPLETE —
   LIVE-VERIFIED GREEN.** Boot 1 s (SteamVR-state-independent, interop-free);
   expected log order observed exactly: registry written (disp=1) → init2 ok
   (token=1) → registry removed → versions valid → FnTable → **device 0
   class = 1** → manufacturer "Valve", model "Index", serial "LHR-C69559AF"
   (all err=0) → **RT 2016x2240** → adapter 0 → freq 120.0 Hz → **pose
   valid=1 connected=1 result=200** → `OpenVR bootstrap COMPLETE`. S2c stack
   alongside: delta=0 every window, ~30 replay passes / 10 s, avgMs ~16.5,
   pin saves ≈ restores ≈ bbRtSets — zero interference from the bootstrap.
   **Three data anomalies observed (open items for S4-2/3, none blocking):**
     (i) `Prop_UserIpdMeters_Float` (2003) returned **2.0000 m** (expect
     ~0.065) — the SAME slot returned freq=120 correctly, so not ABI;
     suspect a deprecated/quirked property in this SteamVR build. S4-3
     should read IPD from GetEyeToHeadTransform's translation magnitude
     instead — NOTE that is slot 4, struct-return (48 B) → i386 hidden return
     pointer becomes the first ARG; more ABI care needed.
     (ii) proj raw L horizontal = 0,0 (R eye fully sane) on the first
     sample — re-sample when settled; possibly wake-transition.
     (iii) pose matrix row 0 garbage (rows 1-2 sane, valid=1) on the one
     sample — the lighthouse log showed "IMU went off scale" churn all day;
     re-sample settled (S4-3 polls continuously anyway and will validate).
   **S4-2 layout verification DONE (2026-10-04, agent session): the served
   IVRCompositor_022 FnTable = 46 slots** — RE of the builtin vrclient.dll
   create_winIVRCompositor_IVRCompositor_022_FnTable (create-table entry at
   0x101f24d8 → builder 0x1000e8c0, thunk array 0x2e0 bytes = 46×16) confirmed
   the method order matches the pinned 1.16.8 header class for slots 0–45
   (SetStageOverride_Async … GetPosesForFrame are 024+ additions, NOT served —
   never touch slot 46+). Submit=5, PostPresentHandoff=7,
   GetVulkanInstanceExtensionsRequired=39, GetVulkanDeviceExtensionsRequired=40;
   same ECX-preset thiscall thunks as IVRSystem — call WITHOUT self. The
   hand-rolled OpenVR POD types were replaced with the vendored header itself
   (`src/carrier/openvr_fntables.hpp` includes `vendor/openvr/openvr_1.16.8.h`
   — the ONLY vendored header now; the SDK 2.15 `openvr.h` (026/029 layouts,
   not served) was deleted 2026-10-04). FnTable structs stay hand-defined
   because the 1.16.8 header predates the SDK's FnTable section; every used
   slot is locked with static_asserts.
2. **Per-eye LDR capture — IMPLEMENTED 2026-10-04 (S4-2, pending live run):
   `src/carrier/hmd_submit.cpp` (conf `openvr_submit=on`, default off;
   requires `openvr=on`; deployed conf is manual-edit).** At the 1→2 boundary
   the backbuffer (pass-1's final LEFT composite) is blitted into carrier
   texture L; at the 2→0 boundary the backbuffer (pass-2's final RIGHT
   composite) is captured BEFORE the monitor-pin restore, then
   `Submit(Eye_Left)` + `Submit(Eye_Right)` (TextureType_DirectX,
   ColorSpace_Gamma, DEFAULT-pool RT-usage textures created from the
   backbuffer's own desc) + `PostPresentHandoff` — all on the render thread
   via eye::set_pass. No WaitGetPoses in v1 (it would re-pace the 30 fps
   loop onto the 120 Hz HMD cadence; add it only if the run shows
   AlreadySubmitted). Fail-soft every step; every DISTINCT Submit error is
   logged once per state change, the 10s window reports captures/submits/
   skips. **RUN 7 (2026-10-04, first live run): CRASHED ~20 ms after the
   first submit pair — root-cause isolated by log timing.** The pair ran
   during the opening credits at frame ~7: both eyes returned **101
   DoNotHaveFocus** (clean IPC round-trips, 6-8 ms each — the compositor
   throttles unfocused submits to 10 Hz; no watchdog abort this time, the
   crash was IN-PROCESS). The very next call in the sequence was
   **PostPresentHandoff — the only never-before-exercised call, never
   logged, and documented to "access the Vulkan queue"** — invoked mid-
     frame (inside the game's SubmitToGPU) with DXVK's boot-time interop
     deliberately disarmed. Captures were fine (the pin-proven StretchRect
   mechanism, one L + one R blit completed). Fix shipped for run 8:
   PostPresentHandoff REMOVED (it is optional per openvr.h — only needed
   when the app can't call WaitGetPoses right after Present, which we
   don't call yet; revisit only together with the WaitGetPoses/pacing
   design), and failed submit pairs now back off to one attempt per 0.5 s
   (a 101 pair costs ~14 ms of the 16 ms frame budget in IPC; a successful
   pair resumes full rate). **101 DoNotHaveFocus is expected during boot
   (dashboard/other app holds compositor focus) and is NOT by itself a
   failure — the questions for run 8 are: (1) does the game survive to
   gameplay, (2) does focus ever arrive (log shows it as a LEFT/RIGHT
   Submit -> 0 transition), (3) if it stays 101 forever, the focus
   investigation moves to the SteamVR side (scene-focus assignment for
   system.generated.wine-preloader, dashboard holding focus).** If the
   crash recurs at the same point WITHOUT PostPresentHandoff, the 101-path
   Submit wrapper itself is the suspect and the next step is gating submits
   until focus exists.
   **RUN 8 (PostPresentHandoff removed): CRASHED AGAIN — deeper root cause
   now isolated. The log ends AT the first Submit enter-log (frame ~3): the
   `LEFT Submit -> ...` result line never appeared, and vrserver saw the
   process disconnect ~30 ms later (in-process death, no malformed IPC).
   Difference from run 7: focus arrived immediately this time, so Submit
   got PAST the early DoNotHaveFocus check — into the D3D9→Vulkan texture
   conversion, which only exists if DXVK's OpenVR interop was armed AT
   DEVICE CREATION (game boot, before the carrier exists). CONCLUSION:
   design option (b) — runtime-interop Submit with the boot interop
   disarmed — is DEAD, live-proven twice (run 7 died at the first queue-
   touching call after the unfocused pair; run 8 at the first focused
   Submit). Shipped for run 9 = option (c): conf `openvr_boot_interop=on`
   (default off) — the LAUNCHER (not the carrier — too late) writes the
   three `HKCU\Software\Wine\VR` values BEFORE CreateProcessW so DXVK's
   d3d9 arms its boot interop at device creation (the intended Valve path
   for 32-bit D3D9 VR apps under Proton). RUN-6A DISCIPLINE applies:
   SteamVR must be FULLY UP and stable before launching with it on, or the
   game can hang pre-main-loop (run 6a). The carrier's attach-time registry
   window stays as-is (serves vrclient; deleting the key post-init2 is
   harmless — the device is already armed, and the launcher re-arms next
   boot). Expected run 9 signatures: launcher line `openvr: boot-time DXVK
   interop ARMED`, then submits returning 0 (or 101 until focus, then 0).
   If it STILL crashes with the interop armed, the next suspects are the
   mid-frame submit call site (move Submit to the Present hook, engineering
   list item 4) and the texture-format assumption (X8R8G8B8 RT textures).
   **RUN 9 (boot interop armed): GAME DIED IN EARLY BOOT — RUN-6A
   SIGNATURE REPRODUCED ON PURPOSE, and the missing variable is now
   clear: SteamVR STABILITY.** Launcher: interop ARMED (disp=1, ok) →
   game started → `boot marker never ticked twice (rc=5)` ~200 ms later —
   rc=5 = ReadProcessMemory access denied = the process was ALREADY DEAD,
   carrier never attached (carrier log still run 8's). vrserver shows the
   launch window INSIDE an HMD wake transition: `16:58:38 leaving standby`
   → launch `16:58:44.7` → `16:58:53 entering standby`; no wine-preloader
   connection ever reached vrserver — DXVK's interop path died at boot
   before/during its connect, mid-transition. CONCLUSION: armed+transition
   = guaranteed early boot death (runs 6a, 9); armed+quiescent = the one
   UNTESTED cell = run 10. Fix shipped: human timing is not reliable
   enough (the HMD wakes/sleeps on its own), so launch.sh now GATES the
   launch when `openvr_boot_interop=on`: parses vrserver.txt's standby
   lines, requires the HMD AWAKE (last line "leaving standby") with no
   transition for ≥8 s, polls up to 120 s with wake-the-HMD hints, and
   ABORTS instead of launching into the known-crash state. Gate verified
   against the real log (correctly reports standby). For run 10: SteamVR
   up, HMD ON/worn (so it stays awake), `./launch.sh` — the gate holds
   until stable. If run 10 STILL dies in early boot with the gate
   satisfies, option (c) is dead too for this game → plan B territory:
   OpenXR + D3D11 + sysmem round-trip (docs/s4_handover.md §S4), or RE of
   DXVK's interop-arm crash path.
   **RUN 10 (gate satisfied): STILL DIED IN EARLY BOOT — OPTION (c) DEAD,
   THE D3D9-IN-PROCESS-OPENVR TEXTURE PATH IS CLOSED (runs 7-10).** Gate
   passed (vrserver: HMD awake since 17:03:12, launch 17:03:22 — 10 s
   quiet, no transitions; launcher disp=2, ARMED), yet the game died ~200
   ms in (rc=5, carrier never attached, no SteamVR connection ever
   reached). Even quiescent, the armed DXVK d3d9 interop kills this game
   before the main loop — the failure is in the armed path itself, not
   timing. VERDICT: every cell is covered — unarmed Submit crashes in the
   D3D9→Vulkan conversion (runs 7/8), armed interop kills early boot
   (runs 9/10, gated or not). ROOT CAUSE across all of S4-2's failures:
   forcing the game's own D3D9 device to interoperate with SteamVR via
   Vulkan external-memory sharing, in-process, under Proton. The fix is
   architectural: SEVER the game's D3D9 device from VR entirely — capture
   to sysmem (proven: dump_surface) and let something else own the VR
   session. Candidate architectures (decision pending, 2026-10-04):
     (A) in-process OpenXR: 32-bit wineopenxr + carrier D3D11 device + sysmem
         upload into an OpenXR swapchain. Unknown: does SteamVR's OpenXR
         runtime serve 32-bit clients (32-bit+D3D11 never tested; the
         wineopenxr64.json registration is broken and must be repaired
         first). All in-process wine-VR coupling remains, just on a
         different API.
     (B) OpenVR + own Vulkan device: keep the PROVEN S4-1 bridge, create a
         carrier-owned Vulkan instance/device, upload from sysmem, submit
         TextureType_Vulkan (VRVulkanTextureData_t). Severs D3D9 from VR,
         but hand-rolls the Vulkan external-memory sharing — the fragile
         kind of code this whole saga warns about.
     (C) out-of-process relay: a 64-bit NATIVE LINUX helper owns the VR
         session (native OpenXR or openvr_api.so, 64-bit — the
         best-supported SteamVR client class on Linux); the 32-bit carrier
         captures the eye pair to a POSIX shared-memory ring buffer and
         the helper uploads to its own textures and submits at HMD cadence
         (independently of the game's 30 fps — big pacing win). HMD pose
         flows back through the same shm for S4-3/S5. No wine-VR coupling
         at all; every piece (capture, shm, pose) is individually simple.
         Cost: a second process + shm protocol (launch.sh spawns it).
   Deployed conf reverted to bootable: openvr_submit=off,
   openvr_boot_interop=off (both are live-proven game-killers as of
   2026-10-04). The S4-1 bridge (openvr=on) stays — it is proven safe and
   remains the diagnostics/facts channel.
   **PREFIX MIGRATION (2026-10-04 evening): the old prefix was GE-Proton-
   created (GE-Proton11-7 in compatibilitytools.d) while launch.conf runs
   Proton Experimental — every run was Experimental wine/DXVK against a
   GE-stamped prefix that Proton kept half-upgrading (openxr manifests
   appearing mid-run, registry writes not persisting). NEW CLEAN PREFIX at
   `/home/laylb/Games/mercenaries-2-clean` (launch.conf updated; game +
   saves copied at the SAME C:\ paths; the game needs NO registry transplant
   — its EA Games/SecuROM keys are EMPTY sections, the game is registry-
   independent). Old prefix kept untouched as fallback (revert launch.conf
   to switch back). Clean-prefix first-run facts: Proton 11.0-100 prefix,
   SteamVR-up setup installed C:\vrclient, syswow64 openvr_api_dxvk.dll +
   wineopenxr.dll, C:\openxr\wineopenxr64.json, and BOTH Khronos registry
   views — which is Proton's OWN behavior on a clean prefix.**
   **OPTION A (in-process 32-bit OpenXR) VERDICT: DEAD — structurally
   impossible on this machine, proven from three directions (2026-10-04):**
     1. The native OpenXR runtime is SteamVR's `bin/linux64/vrclient.so`
        (per ~/.config/openxr/1/active_runtime.json) — 64-BIT ONLY;
        SteamVR ships no linux32 directory and NO 32-bit OpenXR runtime
        library exists anywhere on the machine (find: only 32-bit
        LOADERS under Proton dirs, never a runtime).
     2. wineopenxr is a LOADER PROXY (objdump NEEDED: libopenxr_loader,
        ntdll), not an OpenVR bridge — the 32-bit chain
        (syswow64 wineopenxr.dll → i386 wineopenxr.so → 32-bit loader)
        terminates at a 64-bit-only runtime .so.
     3. Proton's own setup only registers the 64-bit manifest on a clean
        prefix (both ActiveRuntime views → wineopenxr64.json).
   Bonus fact: SteamVR's OpenXR "runtime" IS vrclient.so — OpenXR is
   bridged to the same OpenVR IPC our S4-1 bridge already speaks. OpenXR
   in-process would have bought nothing but a different API surface.
   REMAINING PATHS: (C) out-of-process 64-bit helper (native OpenXR via
   vrenv.sh, or 64-bit wine OpenVR — both SteamVR-supported 64-bit client
   classes) + shm frame relay from the 32-bit carrier; (B) OpenVR in-
   process + TextureType_Vulkan with a carrier-owned Vulkan device +
   sysmem upload. C recommended.
   **DECISION (2026-10-04 evening): option C, with SHARED HANDLES instead of
   sysmem — probe-verified viable.** The design: the carrier creates its two
   capture textures as D3D9Ex SHARED textures (CreateTexture pSharedHandle;
   RT usage, DEFAULT pool — the same StretchRect capture path hmd_submit
   already uses, plus an event-query GPU sync before publishing each frame —
   probe-proven REQUIRED: without the sync the reader raced the queued
   StretchRect and saw zeros), and publishes just the HANDLE VALUES + frame
   index over a small POSIX shm control block. A 64-bit wine HELPER in the
   same prefix (first-class DXVK D3D11 + openvr_api_dxvk x64 app — the
   SteamVR-supported class; run-6a landmine does not apply, the game process
   is never at stake) opens them via OpenSharedResource, owns the OpenVR
   session, and submits at HMD cadence with fresh poses. HMD pose flows back
   through the same shm for S4-3. The game process does ZERO VR work — it
   stays in the proven-clean run-6b state permanently.
   PROBE A RESULT (tools/vr_share_probe/, 2026-10-04, clean prefix):
   **PASS** — writer32 (i686 d3d9ex, shared RT texture, Clear+StretchRect
   fill, event-query sync, self-check readback) → reader64 (x64 d3d11,
   OpenSharedResource, staging readback): 512/512 samples verified,
   cross-process AND cross-bitness, same prefix. The fill mechanism is the
   exact carrier capture shape. (Note: probe A initially FAILED all-zeros;
   root cause was the missing GPU sync, NOT the sharing — the writer
   self-check + sync fixed it. Any carrier capture-to-shared-texture code
   MUST issue the event-query flush before publishing each frame.)
   PROBE B (tools/vr_helper_probe/, PENDING USER RUN): the helper core —
   64-bit D3D11 + OpenVR x64 textbook loop (WaitGetPoses → Submit L/R →
   PostPresentHandoff), red-tinted LEFT / cyan-tinted RIGHT checkerboard in
   the HMD for 15 s. SteamVR up + HMD on; run tools/vr_helper_probe/run.sh.
   If probe B passes, the relay build-out order: (1) helper binary
   (session + OpenSharedResource + submit loop + pose publisher),
   (2) carrier: switch hmd_submit's textures to shared handles + query sync
   + shm handle publisher (pose consumer comes with S4-3), (3) launch.sh
   spawns the helper, (4) live run.
   Error → next step (updated for run 8):
     - `0 None` — image in the HMD, S4-2 core done (pacing/S4-3 next).
     - `106 SharedTexturesNotSupported` / `105 TextureUsesUnsupportedFormat`
       / `1 RequestFailed` — the D3D9 texture path needs DXVK's interop:
       go design (c), re-arm `openvr_vulkan_instance_extensions` behind a
       SteamVR-stability gate at the next boot (run-6a knowledge applies).
     - `108 AlreadySubmitted` — the runtime wants WaitGetPoses per frame:
       add it at the 2→0 boundary and measure the pacing cost.
     - `101 DoNotHaveFocus` / `103 IsNotSceneApplication` — session-level
       (focus stolen or Scene init lost); investigate vrserver state.
   Original design options kept for reference: carrier D3D9 textures
   (DEFAULT pool, shared handles if the interop needs them), filled at the
   pass boundaries from the backbuffer (see the design observation above),
   or submit the fp16 RTs if the interop supports them (unlikely to look
   right — they are pre-tonemap; verify on the monitor first).
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

- `g_RenderQueue2` (2D/overlay) consumption timing vs Present — needed for
  the S4 compositor's HUD handling; add counters when S4 starts.
- Shaders without `viewContextData` (explicit `g_ViewProjMtx`, `LocalToProj`,
  `Mvp`/`TexGen`, rain) are not rewritten and lag the pan — check billboards,
  rain, particles, quads before assuming the pair is geometry-correct
  everywhere (stereo_design.md §S2 item 2).
- Shadow-map basis is the light's, not the camera's — shadows will lag the
  eye offset; assess visually once in the HMD.
- `view_asym_x/y` sign convention vs the HMD runtime.
- Why `ViewManager_Update` never fired in the S1 traced run (curiosity, not
  a blocker).
