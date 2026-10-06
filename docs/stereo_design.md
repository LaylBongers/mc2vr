# Stereo Submission Design

Design for dual-eye world rendering + HMD presentation (via a separate 64-bit OpenXR host process). This doc is
about OUR implementation; what the game itself does is in `reverse_engineering/` (camera/view/shader constants:
`reverse_engineering/view_and_camera.md`; frame chain: `reverse_engineering/render_path.md`).
Mechanism rules and hook list: `launcher_plan.md`. Overview diagram: `render_diagram.svg`. Code: `src/carrier/view_rewrite.cpp`.

## Status

| Phase | State |
|---|---|
| S0 loop-body RE | complete |
| S1 draw-camera hunt | complete — the camera is only reachable at the GPU boundary |
| S2 per-eye injection (incl. S2c second draw pass) | **COMPLETE + LIVE-VERIFIED 2026-10-04**: `stereo` camera channel, deterministic per-frame L/R pair, parallax-proven (−7px, SAD 2.16 vs 3.28), stable monitor pin. Milestone record in git history (`git log --follow -- docs/s2c_handover.md`) |
| S4 HMD presentation | **S4-0..S4-4 COMPLETE + LIVE-VERIFIED 2026-10-05**: separate 64-bit OpenXR/D3D11 host, shared-handle images, IPC, head-tracked 3D in the HMD (full VP replacement, §S4-4). S4-5 (events/pacing/HUD) not started; brief + task list in `s4_handover.md` |
| S5 motion controls | not started |

## Verified-in-game record

- `stereo` camera channel (2026-10-04): the derived right axis is unit-length and tracks camera yaw
  ([1,0,0] → [-1,0,0] through a 180° turn), 100k+ rows rewritten per gameplay window; menu flips show
  right=[0,0,0] (cache unseeded until the first main-pass camera upload — expected).
- Open RE questions behind this design (camera-matrix writer hunt, stub callbacks) are tracked in
  `reverse_engineering/view_and_camera.md` § Open RE items.

## Premises (from reverse engineering)

Details and evidence in `reverse_engineering/view_and_camera.md` and `reverse_engineering/render_path.md`.

- Producer side is plaintext and main-thread only; between the ring and the draw records sits the SecuROM-VM'd
  packet interpreter (stub `0x0050f660` at `0x004c99f9`); `PgPrimitive_SubmitToGPU` (`0x00855690`) →
  `BeginSubmit` → `RenderCmd_ExecuteStream` (`0x008569d0`) → `EndSubmit` is plaintext.
- The draw camera is external to the view system: it only crosses plaintext code as D3D constant uploads, so
  **the GPU boundary is the only per-eye injection point.** Do not re-litigate the producer side (duplicating
  views/staging cannot work: the consumer never reads view camera data and derefs post-walk).
- No fixed-function projection; the projection is folded into the `viewContextData` VP rows, so per-eye
  asymmetric projection is an edit to the same rows.
- `LtiRenderer_EndSubmit` already StretchRects RT0 → backbuffer (`LtiRenderer+0x3ea4`) whenever they differ —
  an existing RT→backbuffer copy path the capture can co-opt.

## The view channel (implemented)

The visible view lives in the VS constant `viewContextData` (layout: `reverse_engineering/view_and_camera.md`
§ `viewContextData` layout: VP rows 0..3, optional camPos, optional world-fixed extra row, row-major,
`clip_i = dot(VP_row_i, worldpos)`). The count-6 extra row is left alone.

**Rewrite** (device VmtHook on `SetVertexShaderConstantF`, slot 94): for a
world-space pan `D = (dx,dy,dz)` — camPos `xyz += D` (per-pixel effects follow
the eye) and every VP row `w -= dot(row.xyz, D)` (rigid world shift on screen;
a uniform clip-w shift does NOT work, the divide scales it per-vertex).
Rewriting is done on a scratch copy returned to the driver call — the game's
upload buffer may alias the persistent per-view record and is never modified.

**Stereo offsets** (`view_row_rewrite=stereo`): `D = ±right·IPD/2`. The camera
right axis is derived in the hook from the raw (pre-rewrite) uploads:
VP row0.xyz = P00·right (view row0 = camera right), so
`right = normalize(row0.xyz)`; the cache refreshes on every main-pass row0
upload (at most one frame old; offscreen passes must not seed/refresh it —
the shadow pass's basis is the light's). Until S2c supplies real per-eye draw
passes the eye sign alternates every `view_stereo_hold` seconds (A/B check:
shadows/materials must stay glued at each eye). Asymmetric per-eye projection
is an edit to the same rows: NDC centre shift `e_k` lands in row_k.w as
`|row_k.xyz|·e_k·eyeSign` (`view_asym_x/y`, default 0 — real values and sign
convention come from the OpenXR runtime via the host in S4).

**Exact registers come from the game's own resolver, not shape matching.** A MidHook at the
upload gate (`MC2_VCD_UPLOAD_CMP` `0x00855a78`, EDI = current technique)
publishes its resolved map (`+0xd4`/`+0xd8` viewContextData reg/count,
`+0xdc`/`+0xe0` ViewProj reg/count); the device hook runs immediately after on
the same thread.

**Pass gate**: shadow-map, reflection and other offscreen passes upload their
own `viewContextData` (the shadow pass's "camera" is the light). Shifting them
moved shadow maps relative to their receivers (visible shadow fading). The
rewrite therefore applies only while RT0 (observed via device `SetRenderTarget`,
slot 37) has the backbuffer size. Seen RT0 sizes: 2560x1440 main; skipped:
1024x4096 shadow atlas, 512², 128², 64², 853x480, and the 1280x720 → 1x1
downsample chain. Caveat: an offscreen pass with exactly the backbuffer size
would be shifted (none seen); if render resolution ever differs from the
backbuffer, key the gate on RT identity. Shadow *receivers* look up in world
space (the VS passes world position to the PS), so they are eye-invariant.

**Controls**: `view_row_rewrite`, `view_row_amp`, `view_ipd`, `view_stereo_hold`, `view_asym_x/y` — documented in `conf/mc2vr.conf`. The real per-eye offset is `D = ±right·IPD/2` (≈0.032 m).

## Architecture

**GPU-boundary per-eye injection**: the frame renders once per eye. During an
eye pass the `viewContextData` registers in the uploads are rewritten for that
eye (pan + asymmetric-projection VP edits). The second draw pass replays the
frame's command stream through the plaintext interpreter. The eye images are handed
to a separate OpenXR host process (§S4) that presents them to the HMD. (A producer-side design —
duplicating each view's element with shadow ViewEntry/staging — cannot work:
the consumer never reads view camera data.)

## Remaining phases

### S2 — remaining work

1. ~~Real per-eye offsets from HMD pose~~ — DONE in S4-4 (§S4-4).
2. **Shaders without `viewContextData` are not rewritten** and will lag the
   pan (not yet observed as visibly wrong — check billboards, rain, particles,
   quads before implementing): explicit `g_ViewProjMtx` (same per-row `w` shift);
   `LocalToProj` (view folded in per object — needs the view-space eye offset,
   `clip.x -= P00*e.x`, with P00 derivable from cached VP rows); `Mvp`/`TexGen`; rain.
   Shader addresses: `reverse_engineering/view_and_camera.md`.
3. PS-side camera data (`cameraPos` c92, texgen matrices are mono) — hook slot 109 if
   reflections/shadows skew at IPD scale (see Open questions).

### S2c — second draw pass (COMPLETE 2026-10-04, live-verified)

Streams carry no draws (`view_and_camera.md`), so the per-eye pass re-invokes
`PgPrimitive_SubmitToGPU` wholesale (`frame_replay`, InlineHook at entry — the record walk
re-runs state + draws; VCD uploads re-issue through the slot-94 rewrite, which `eye_pass` keys on).
Code: `src/carrier/debug/stream_capture.cpp` (stream tap + replay hook), `src/carrier/eye_replay.cpp`.

- **S2c-0** capture + census: full opcode table + interpreter facts on the
  `RenderCmd_ExecuteStream` Ghidra plate (dedupe global `0x011697b8` → replay must use copied pointers).
- **S2c-1** second pass: was vsync-capped at 30 Hz (2 blocking presents exceed the 60 Hz budget);
  S4-5 fix LIVE-VERIFIED 2026-10-06: `vsync=off` forces `D3DPRESENT_INTERVAL_IMMEDIATE` via the
  Direct3DCreate9-thunk + IDirect3D9-VmtHook gate — game now ~135 Hz (pass 2 costs 3.2 ms).
- **S2c-2** `eye_pass` (pass 1 = LEFT, pass 2 = RIGHT) + `eye_rt` (pass-2 device-level
  SetRenderTarget(0)/StretchRect redirect to a carrier backbuffer-sized eye RT). Without the monitor pin the two per-frame EndSubmit copies
  alternate L/R on the monitor (rapid horizontal oscillation = working temporal stereo).
- **Acceptance (run 3)**: 5 gameplay BMP pairs measure a consistent −7px horizontal parallax
  (SAD 2.16 vs 3.28 at shift-0; `debug_eye_dump_frames`, `tools/analyze_dumps.py`).
- **Monitor pin** = backbuffer SNAPSHOT before pass 2 / RESTORE after (suppressing backbuffer writes
  is wrong under SwapEffect=DISCARD — stale driver page, live-observed); stable and free.
- Details: `s4_handover.md`; run-by-run record in git history
  (`git log --follow -- docs/s2c_handover.md`).

### S4 — Presentation / HMD runtime (OpenXR host process) — S4-0..S4-4 COMPLETE, live-verified

**Why a host process**: the game is 32-bit + D3D9 (DXVK); Valve's OpenXR driver has no 32-bit+DX9 support. A
**64-bit host exe in the same Wine prefix** (`src/host/` → `build/win64/bin/mc2vr_host.exe`, statically
linked, vendored OpenXR SDK 1.1.54) owns the OpenXR session (D3D11 binding), frame loop (`xrWaitFrame` at HMD
cadence, ~120 Hz) and event pump. The carrier only captures images, injects the camera and exchanges data.
OpenVR is not used anywhere.

```
game (i386, D3D9/DXVK)                              host (x86_64, D3D11/DXVK)
 carrier ── eye copies ──► shared textures ──────────► OpenSharedResource ─► blit ─► OpenXR swapchains
    └── commands (FRAME_READY+poseId) ─► IPC ◄── state seqlock (pose/FOV/IPD/session), events ──┘
```

**Host source map** (`src/host/`): `main.cpp` (args `--mock --frames N --xr-debug`), `xr_session.cpp`
(instance/session/swapchains/event pump/frame loop, pose history, Wine VR registry fixups), `mock.cpp` (no
runtime: synthetic pose + blit smoke test; used by the selftest), `d3d.cpp` (D3D11 device on the runtime LUID),
`eyes.cpp` (test pattern fallback), `shared_eyes.cpp` (handle open cache, mirror window, `latest()` seam),
`submit.cpp` (swapchain blit), `ipc.cpp`, `log.cpp`, `pose.hpp`. Carrier: `src/carrier/eye_share.cpp` (capture
ring), `src/carrier/ipc.cpp`. Protocol: `src/common/mc2vr_ipc.h`.

**Per-frame assets**: game main RT = pass-1 LEFT, pre-composite fp16 HDR 2560×1440 (A16B16G16R16F); carrier
eye RT = pass-2 RIGHT, same format; the **tonemapped LDR finals are on the backbuffer at the pass
boundaries** (gameplay ends each pass with one DRAW into RT0=backbuffer; menus/loading use a mainRT→backbuffer
StretchRect — boundary capture works for both). Capture = StretchRect backbuffer → shared ring slot at 1→2
(LEFT) and 2→0 (RIGHT, BEFORE the monitor-pin restore). Swapchain: 2560×1440 X8R8G8B8, SwapEffect=DISCARD (never
suppress backbuffer writes). Each submit begins with "Present prev" ⇒ 2 Presents/frame, ~30 fps; every submit
waits on all prior GPU work (event-query spin in `LtiRenderer_BeginSubmit`).

**Monitor pin** (`eye_monitor_pin=on`): snapshot backbuffer → snapshot RT at 1→2, restore after pass 2 via
`device::blit_surfaces` (original-method trampoline). Both Presents show LEFT; free, stable; keep it.

**IPC (protocol v1)**: one section created by the host (refuses if one exists), name `mc2vr_ipc_v1` (env
`MC2VR_IPC_NAME`). `Mc2IpcState` seqlock (host writer): pose/FOV (OpenXR **angles in radians**, not tangents)
per eye, IPD, session state, recenter counter, `hostFrame` (poseId = hostFrame+1). SPSC rings: events
host→carrier (session state, exit, recenter), commands carrier→host (`CONFIG`, `FRAME_READY{x=frameId
y=handle a=slot b=eye c=w d=h e=poseId}`, `SHUTDOWN`). Both sides watch each other's process (carrier pid in
header; `hostExiting` flag). Launcher spawns the host before the game and waits for `mc2vr_host: ready`
(30 s, non-fatal; no host ⇒ the game runs mono+stereo-on-monitor as before). Carrier connects in stage 1
(non-fatal), 250 ms monitor thread logs state transitions; the render thread only does lock-free seqlock reads.

**Shared-handle images (S4-2, probe `tools/probe/run_shared_handle.sh`, all-PASS)**: DXVK D3D9 legacy
`pSharedHandle` textures open in DXVK D3D11 (`OpenSharedResource`) cross-process (D3D9Ex/plain ×
A8R8G8B8/X8R8G8B8; → DXGI 87 / 88 with alpha 0xFF). Ring = 4 RT-usage slots/eye (StretchRect needs RT
surfaces); handles travel as plain integers; recreated lazily after device Reset. Cross-process sync = producer
event-query flush only (no fence on legacy handles): **`GetData` must pass `D3DGETDATA_FLUSH`**; bounded 8 ms
(`syncTimeouts` few/run, benign). Raw-vtable slots that bit us: texture GetSurfaceLevel=18, query Issue=6/
GetData=7, device CreateTexture=23. The host mirror must `CopyResource` into staging before `Map`; the handle
cache is reserved (64) so `Entry*` stay stable.

**Submission (S4-3)**: runtime swapchains are sRGB-only for 8-bit (91 B8G8R8A8_SRGB preferred, else 29), and
`CopyResource` UNORM↔sRGB is illegal, so `submit.cpp` draws a fullscreen triangle sampling the shared texture
through a UNORM-cast SRV and writing through a UNORM-cast RTV (91→87, 29→28): the sRGB-encoded LDR bytes pass
through raw and the compositor decodes them (no double gamma). Shaders compiled at startup via dynamic
`d3dcompiler_47.dll`. Fallback on any failure: the S4-0 test pattern. `stretch=false` = aspect-fit letterbox
(frames without a pose), `stretch=true` = fill (HMD-pose frames, see §S4-4). 10 s `submit:` stats lines are the
acceptance evidence (fresh = per-eye blits of new carrier frames, reused = re-submits of the newest pair).

**OpenXR under Proton (S4-0 gotchas, handled in `xr_session.cpp::init_wine_vr_registry`)**: (1) must run through
`proton run` (plain `wine` ⇒ wined3d: "doesn't support IDXGIVkInteropDevice"); (2) wineopenxr negotiation fails
(-6, runtime "lacks" every extension) unless `HKCU\Software\Wine\VR` exists, `wineopenxr_init_registry()` ran and
DWORD `state`=1 (normally published by vrclient_x64 for an OpenVR app; state=2 fails) — the host creates these;
(3) `--xr-debug` writes loader tracing to `mc2vr_host_xrloader.log`, stdout is lost under proton so read
`mc2vr_host.log`; (4) "stuck in SYNCHRONIZED" has been SteamVR itself crashed — restart it first. Session: LOCAL
space, per-eye swapchains 2016×2240 ×3 images, runtime `SteamVR/OpenXR 2.17.10`, `ActiveRuntime` →
`C:\openxr\wineopenxr64.json`.

**Open risks / items**: HUD in-composite or not and `g_RenderQueue2` timing (S4-5); pacing step 1
live-verified (vsync unlock, see below); if per-eye RTs ever can't differ
at the D3D level, fall back to single-backbuffer interop blit.
Session events (S4-5): the slot-5 PostUpdateHook drains the host event ring
(state/recenter logged; `MC2VR_MSG_EXIT` → one-shot `WM_CLOSE` to the game's
root window — the engine pump's quit path is VM-protected so the message is
the signal; the host pushes EXIT only for runtime-initiated ends via its
`selfExit` flag). Focus lost needs no carrier action: host submission is
gated on `shouldRender` (zero layers while not VISIBLE/FOCUSED).

### S4-4 — HMD camera replacement (COMPLETE, live-verified 2026-10-05)

Supersedes the translation-only pan + `view_asym` plan for HMD rendering (those
stay for the `stereo` verification mode). D3D clip for a standard view/projection:
`clip = [a·x_v + c·z_v, b·y_v + d·z_v, A·z_v + B, z_v]`, `x_v/y_v/z_v = dot(R/U/F, p−C)`.
So the four VP rows are `row0 = aR + cF`, `row1 = bU + dF`, `row2 = A·F`, `row3 = F`
(xyz), with `w = −dot(xyz, C)` (row2.w additionally `+B`). Decomposition (`decompose`):
`F = row3.xyz` (must be unit — else the technique is passed through and counted),
`R,a` from row0 minus its F component, `U,b` likewise, `c,d` the F components,
`A = row2·F` (row2 must be ∥ F), `C` from solving `clip.x=clip.y=clip.w=0` (3×3
Cramer — independent of the optional camPos row), `B = row2.w + row2·C`.
`view_row_rewrite=hmd_identity` rebuilds the game's own camera and logs the max
residual. The algebra lives in `src/carrier/vp_camera.{hpp,cpp}` (pure math on `src/common/vec_math.hpp`, shared with the host); native unit test `tools/test/test_vp_camera.cpp` (build line in its header) covers decompose/rebuild round trip, identity eye, eye-shift and head-turn directions, scale.

HMD eye → game camera (`apply_hmd_eye`): XR LOCAL vectors map x→R, y→U, −z→F of the
GAME camera (the game camera is the body; the HMD is an offset on it, so mouse/stick
turning still works); position `C' = C + map(eyePos)·view_world_scale`; axes
`R',U',F' = map(q·x̂, q·ŷ, q·(−ẑ))`; projection from `tan()` of the OpenXR angles:
`a=2/(tR−tL), c=−(tR+tL)/(tR−tL)` (y likewise); `A,B` kept so depth/fog/soft-particle
inputs are unchanged. The render target keeps its 16:9 size; the squeezed XR frustum
is un-squeezed by the host's full-image stretch (exact inverse), no letterbox.

Pose consistency: the carrier snapshots the host state once at pass-1 start (both eyes
use it) and tags frames with `poseId = hostFrame+1` (`FRAME_READY.e`); the host keeps
a 256-entry published-view history and submits the projection layer with the pose+FOV
of that id, so the compositor reprojects from what the image actually contains.

Open: unit scale (`view_world_scale`, unverified), engine culling against the game
frustum, non-`viewContextData` shaders / PS camera data (rotation exposes these),
split VP uploads (counted: `view/hmd: split=`), handedness/sign validation live.

### S5 — Motion controls (separate track)

Follows the logic-mod track in `launcher_plan.md` (XInput stubs
`0x00a64d56/0x00a64d5c`, idle-reset buffer pair `0x017d30e8`/`0x00f7fb90`
first). Pose/input marshal point is the slot-5 hook (S4); controller poses and button/axis state arrive from the host's OpenXR actions over the same IPC.

## Hook strategy (frame level)

- Frame hook: `GameShell_FrameTick` (InlineHook, per-frame counting + 10 s timing reports). Frame-level slots
  `g_RenderShell` vtable 4/5 (`EndOfFrameHook`/`PostUpdateHook`) are claimed via cloned-vtable swap (proven
  1:1 with frames, survives alt-tab/cutscene/mission load).
- Reuse the game's own timing (`g_FrameDeltaSec`, `g_Dt`); frame pacing for VR should bypass/neutralize the
  adaptive framerate path (`g_FrameratePolicy` / `AdaptiveFramerate_Govern`) so the HMD drives the cadence.
- Never hook via the SecuROM wrapper pointers (runtime-only) or inside `0x01a48000+`.
- Device-vtable hooks that only observe/forward calls are safe (the game's state caches are caller-side in
  the `Dx9_*` functions); ALTERING values at the device level would desync them (`g_RenderStateCache`,
  texture/sampler/RT caches) — alter at the `Dx9_*` function level instead, or rewrite scratch copies as
  `view_rewrite.cpp` does.

## Hook inventory

| Site | Mechanism | Purpose |
|---|---|---|
| Device `SetVertexShaderConstantF` (slot 94) | VmtHook | the view rewrite (scratch-copy upload) |
| Device `SetRenderTarget` (slot 37) | VmtHook (observe only) | main-pass gate |
| Upload gate `MC2_VCD_UPLOAD_CMP` `0x00855a78` | MidHook | publish the technique's exact viewContextData/ViewProj map |
| `RenderCmd_ExecuteStream` opcode `0x008569f5` | MidHook | M3 histogram + S2c stream tap |
| Device `Present` (17) / `Reset` (16) | VmtHook | present params / monitor path (the host, not Present, submits to the HMD) |
| `g_RenderShell` slots 4/5 | cloned-vtable claim | S4 orchestration: pose read, event drain, FrameReady publish (counting no-op now) |
| `SubmitWorldPackets` loop head `0x0048e9ea` | MidHook | M3 view aggregation |
| Stub call `0x004c99f9`/`0x004c99fe` + ~15 plaintext helper entries | MidHook | optional callback tracer (`debug_stub_trace`, see `reverse_engineering/render_path.md`) |

Proven mechanisms: trap-based inline/Mid/Vmt installs (no suspension), device
VmtHook surviving device-lost + `Reset`, slot 4/5 claim 1:1 with frames,
in-buffer-style constant interception where the draw consumes the modified
data. **Never hook**: VM entry stub `0x0050f660`, VM pose-getter thunk
`0x0048bf00` (use plaintext call sites), anything at `0x01a48000+`.

## Open questions

- **Material texgen / PS camera data stays mono for eye 2** (water/sky reflections, blob shadows, shadow
  cascades, PS `cameraPos`, the PS copy of the view record — RE detail in `view_and_camera.md`). The view
  rewrite touches VS camera rows only. No visible issue at 0.05 units; at IPD scale and for the periphery,
  re-check. A later `SetPixelShaderConstantF` hook could shift camera-derived rows by the eye delta (the
  derivation is VM'd, so correctness is not guaranteed).
- `g_RenderQueue2` consumption timing relative to Present (HUD handling needs
  the 2D stream's frame timing; RE side in `view_and_camera.md`) — add queue2 counters when S4 starts.
- GPU sync: every frame begins by waiting for all prior GPU work (event-query spin in `LtiRenderer_BeginSubmit`, see `reverse_engineering/render_path.md`). Per-eye passes inherit it; the pacing design must account for it (S2c replay happens after this point).
- Frame pacing: game vsync-locked 60 Hz (30 Hz with two passes; live-log-proven
  2026-10-05: ~600 Presents vs ~300 frames per 10s — one present-prev Present per pass at
  `LtiRenderer_BeginSubmit`). HMD typically 90 Hz. The host runs at HMD cadence
  independently and re-submits the newest pair with runtime reprojection.
  S4-5 first step LIVE-VERIFIED (2026-10-06): `vsync=off` breaks the game's
  vsync lock (IMMEDIATE interval at CreateDevice/Reset via the Direct3DCreate9 thunk
  InlineHook + IDirect3D9 VmtHook, `src/carrier/device.cpp`) — the game runs ~135 Hz
  (7.4 ms/frame; the wait was the cost, not the GPU). Free-run currently healthy: game
  (135 Hz) outpaces the host (~120 Hz), `pose ids miss=0`, host consumes the newest pair.
  Whether to additionally pace the game to the HMD (slot-4 `EndOfFrameHook`) is the
  remaining decision — run-free is the current default.
