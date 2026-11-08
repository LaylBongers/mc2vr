# Stereo pipeline

GPU-boundary per-eye injection: the frame renders once per eye. During an eye pass the `viewContextData` uploads are rewritten for that eye ([view_rewrite.md](view_rewrite.md)); the eye images go to the [host](host.md) over [shared textures](shared_textures.md).

Premises (proven, do not re-litigate; RE detail: [draw_camera_chain.md](reverse_engineering/draw_camera_chain.md), [view_context_records.md](reverse_engineering/view_context_records.md)):

- The draw camera is external to the view system — it only crosses plaintext code as D3D constant uploads, so the GPU boundary (plus the camera table, [camera.md](camera.md)) are the injection points. Producer-side view duplication cannot work (E2: the ViewEntry pose fields are output channels).
- No fixed-function projection (`SetTransform` is never called); the projection is folded into the `viewContextData` VP rows, so per-eye asymmetric projection is an edit to the same rows.
- `LtiRenderer_EndSubmit` already StretchRects RT0 → backbuffer (`LtiRenderer+0x3ea4`) whenever they differ — an existing RT→backbuffer copy seam.

## Second draw pass (always on; `frame_replay` retired 2026-11-08)

Streams carry no draws, so the per-eye pass re-invokes `PgPrimitive_SubmitToGPU` wholesale (InlineHook at entry — the record walk re-runs state + draws; VCD uploads re-issue through the slot-94 rewrite, which `eye_pass` keys on). Code: `src/carrier/debug/stream_capture.cpp` (stream tap + replay hook), `src/carrier/eye_replay.cpp`. Replay must use copied pointers (dedupe global `0x011697b8`). Pass 1 = LEFT, pass 2 = RIGHT; the replay happens after the frame's GPU-sync point (event-query spin in `LtiRenderer_BeginSubmit`) and costs ~3.2 ms.

## Outputs + capture points

The engine's final frame of each pass lands in the **carrier-owned front** (`src/carrier/render_outputs.cpp`, always on; mechanism and RE in [render_resolution.md](render_resolution.md) §2 and [render_target_table.md](reverse_engineering/render_target_table.md)). The gameplay composite and HUD come in through render-command op 6, with pointers baked per frame from the table. Menus and loading screens come in through `LtiRenderer_EndSubmit`'s main RT → front StretchRect. Pass 2 simply re-renders into the engine's own targets: pass 1 has already been composited and captured by then. Run 2026-11-08 with `eye_rt=off`: both eyes and all post effects correct.

Capture = StretchRect carrier front → shared ring slot at 1→2 (LEFT) and 2→0 (RIGHT). The front carries the engine's requested MSAA (8×), so the capture is also the resolve.

## Monitor

The swapchain (no MSAA) is only the carrier's monitor mirror. At 1→2 the front is resolved into a mirror RT; before every Present the mirror is blitted (aspect-fit) into the swapchain backbuffer, so both per-frame Presents show the current LEFT image. SwapEffect=DISCARD gives no backbuffer persistence, which is why the blit runs before every Present.

Retired 2026-11-08 (git history has them): the pass-2 main-RT → eye-RT redirect (`eye_rt`) and the monitor pin (`eye_monitor_pin`, which snapshotted and restored the backbuffer around pass 2). Its lesson still applies: never suppress backbuffer writes under DISCARD (stale driver page, live-observed 2026-10-04).

## HUD/2D lands in both eyes

`g_RenderQueue2` (`0x00ff3650`, 2D/overlay) has no plaintext consumer (the frame-ctx hands `&g_RenderQueue2` to the VM interpreter). Measured with `src/carrier/hud_timing.cpp`: both queues are fully consumed between SubmitToGPU entry and pass-1's BeginSubmit Present and are FROZEN from `p1` through `b0` — the interpreter builds the 2D/HUD draw records once per frame BEFORE pass 1, and pass 2 re-walks the same record table. The HUD lands in both eyes' composites; the one-eye-HUD scenario is structurally impossible (the host quad-layer fallback is dead). Side finding — queue counter protocol: `+0x10` high16 = pending-unconsumed count cleared by the consumer pre-Present, low16 = ring position frozen during passes, `+0x14` unused. Residual HUD defects: [hud.md](hud.md).

## Known gaps (open, low priority)

- Shaders without `viewContextData` are not rewritten and lag the per-eye camera: explicit `g_ViewProjMtx` (same per-row `w` shift); `LocalToProj` (view folded in per object — needs the view-space eye offset, `clip.x -= P00*e.x`, P00 derivable from cached VP rows); `Mvp`/`TexGen`; rain. Check billboards/rain/particles/quads before implementing (not yet visibly wrong). Shader addresses: [shader_ctab_map.md](reverse_engineering/shader_ctab_map.md).
- PS-side camera data (`cameraPos` c92, texgen matrices) is mono — hook `SetPixelShaderConstantF` (slot 109) if reflections/shadows skew at IPD scale.
- Rendering happens at window resolution (2560×1440) with the projection squeezed to match — decoupling to HMD-native rendering is planned: [render_resolution.md](render_resolution.md).
- Duplicate Present retired 2026-11-08 (live-confirmed: Present = frames). Before pass 1 the carrier sets the engine's `g_SuppressPresent` (`render_outputs::begin_present_skip`, from the SubmitToGPU replay hook), so only pass 2's present-prev runs, after the 1→2 mirror refresh, showing the current LEFT. `SubmitToGPU` clears the flag after BeginSubmit, and the carrier clears it again before pass 2. Only active while the carrier owns the outputs and two passes run.
- If per-eye RTs ever can't differ at the D3D level, fall back to a single-backbuffer interop blit.
