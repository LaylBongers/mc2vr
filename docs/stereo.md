# Stereo pipeline

GPU-boundary per-eye injection: the frame renders once per eye. During an eye pass the `viewContextData` uploads are rewritten for that eye ([view_rewrite.md](view_rewrite.md)); the eye images go to the [host](host.md) over [shared textures](shared_textures.md).

Premises (proven, do not re-litigate; RE detail: [render_path.md](reverse_engineering/render_path.md), [view_and_camera.md](reverse_engineering/view_and_camera.md)):

- The draw camera is external to the view system — it only crosses plaintext code as D3D constant uploads, so the GPU boundary (plus the camera table, [camera.md](camera.md)) are the injection points. Producer-side view duplication cannot work (E2: the ViewEntry pose fields are output channels).
- No fixed-function projection (`SetTransform` is never called); the projection is folded into the `viewContextData` VP rows, so per-eye asymmetric projection is an edit to the same rows.
- `LtiRenderer_EndSubmit` already StretchRects RT0 → backbuffer (`LtiRenderer+0x3ea4`) whenever they differ — an existing RT→backbuffer copy seam.

## Second draw pass (`frame_replay`)

Streams carry no draws, so the per-eye pass re-invokes `PgPrimitive_SubmitToGPU` wholesale (InlineHook at entry — the record walk re-runs state + draws; VCD uploads re-issue through the slot-94 rewrite, which `eye_pass` keys on). Code: `src/carrier/debug/stream_capture.cpp` (stream tap + replay hook), `src/carrier/eye_replay.cpp`. Replay must use copied pointers (dedupe global `0x011697b8`). Pass 1 = LEFT, pass 2 = RIGHT; the replay happens after the frame's GPU-sync point (event-query spin in `LtiRenderer_BeginSubmit`) and costs ~3.2 ms.

## Eye RT + capture points

`eye_rt`: pass-2 device-level SetRenderTarget(0)/StretchRect redirect to a carrier backbuffer-sized eye RT. Gameplay's final composite is a DRAW into RT0=backbuffer (counter-proven: the StretchRect composite was observed-not-redirected); menus/loading use a mainRT→backbuffer StretchRect — boundary capture works for both. The tonemapped LDR finals are on the backbuffer at the pass boundaries (main RT = pass-1 LEFT, pre-composite fp16 HDR 2560×1440 A16B16G16R16F; eye RT = pass-2 RIGHT, same format).

Capture = StretchRect backbuffer → shared ring slot at 1→2 (LEFT) and 2→0 (RIGHT, BEFORE the monitor-pin restore). Swapchain 2560×1440 X8R8G8B8, SwapEffect=DISCARD — **never suppress backbuffer writes** (stale driver page, live-observed).

## Monitor pin (`eye_monitor_pin`)

Snapshot backbuffer → snapshot RT at 1→2, restore after pass 2 via `device::blit_surfaces` (original-method trampoline). Both Presents show LEFT; free and stable. Without it the two EndSubmit copies alternate L/R on the monitor (rapid horizontal oscillation = working temporal stereo).

## HUD/2D lands in both eyes

`g_RenderQueue2` (`0x00ff3650`, 2D/overlay) has no plaintext consumer (the frame-ctx hands `&g_RenderQueue2` to the VM interpreter). Measured with `src/carrier/hud_timing.cpp`: both queues are fully consumed between SubmitToGPU entry and pass-1's BeginSubmit Present and are FROZEN from `p1` through `b0` — the interpreter builds the 2D/HUD draw records once per frame BEFORE pass 1, and pass 2 re-walks the same record table. The HUD lands in both eyes' composites; the one-eye-HUD scenario is structurally impossible (the host quad-layer fallback is dead). Side finding — queue counter protocol: `+0x10` high16 = pending-unconsumed count cleared by the consumer pre-Present, low16 = ring position frozen during passes, `+0x14` unused. Residual HUD defects: [hud.md](hud.md).

## Known gaps (open, low priority)

- Shaders without `viewContextData` are not rewritten and lag the per-eye camera: explicit `g_ViewProjMtx` (same per-row `w` shift); `LocalToProj` (view folded in per object — needs the view-space eye offset, `clip.x -= P00*e.x`, P00 derivable from cached VP rows); `Mvp`/`TexGen`; rain. Check billboards/rain/particles/quads before implementing (not yet visibly wrong). Shader addresses: [view_and_camera.md](reverse_engineering/view_and_camera.md).
- PS-side camera data (`cameraPos` c92, texgen matrices) is mono — hook `SetPixelShaderConstantF` (slot 109) if reflections/shadows skew at IPD scale.
- Deferred (accepted): the second per-frame Present presents identical pinned-LEFT content; with `vsync=off` it is a non-blocking blit — re-evaluate only if compositor pressure is implicated in the staleness issue ([pacing.md](pacing.md)).
- If per-eye RTs ever can't differ at the D3D level, fall back to a single-backbuffer interop blit.
