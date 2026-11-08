# Stereo pipeline

GPU-boundary per-eye injection: the frame renders once per eye. During an eye pass the `viewContextData` uploads are rewritten for that eye ([view_rewrite.md](view_rewrite.md)); the eye images go to the [host](host.md) over [shared textures](shared_textures.md).

Premises (proven, do not re-litigate; RE detail: [draw_camera_chain.md](reverse_engineering/draw_camera_chain.md), [view_context_records.md](reverse_engineering/view_context_records.md)):

- The draw camera is external to the view system — it only crosses plaintext code as D3D constant uploads, so the GPU boundary (plus the camera table, [camera.md](camera.md)) are the injection points. Producer-side view duplication cannot work (E2: the ViewEntry pose fields are output channels).
- No fixed-function projection (`SetTransform` is never called); the projection is folded into the `viewContextData` VP rows, so per-eye asymmetric projection is an edit to the same rows.

## Second draw pass

Streams carry no draws, so the per-eye pass re-invokes `PgPrimitive_SubmitToGPU` wholesale (InlineHook at entry — the record walk re-runs state + draws; VCD uploads re-issue through the slot-94 rewrite, which keys on the pass). Code: `src/carrier/debug/stream_capture.cpp` (stream tap + replay hook), `src/carrier/eye_replay.cpp`. Replay must use copied pointers (dedupe global `0x011697b8`). Pass 1 = LEFT, pass 2 = RIGHT; the replay happens after the frame's GPU-sync point (event-query spin in `LtiRenderer_BeginSubmit`) and costs ~3.2 ms.

## Outputs + capture points

The engine's final frame of each pass lands in the carrier-owned front ([render_outputs.md](render_outputs.md)). It is captured into the shared ring at 1→2 (LEFT) and 2→0 (RIGHT); the capture is also the MSAA resolve. Pass 2 simply re-renders into the engine's own targets, because pass 1 has already been composited and captured by then. The monitor shows the LEFT image through the carrier's mirror, with one Present per frame.

Lesson that still applies: never suppress backbuffer writes under SwapEffect=DISCARD (stale driver page).

## HUD/2D lands in both eyes

`g_RenderQueue2` (`0x00ff3650`, 2D/overlay) is consumed by unlocated code; the frame-ctx block carries `&g_RenderQueue2`. It is not thunk `0x0050c660` ([reverse_engineering/securom_vm.md](reverse_engineering/securom_vm.md)). The HUD/2D is ordinary draw records from the Scaleform renderer, built once per frame in the record build ([reverse_engineering/gui_renderer.md](reverse_engineering/gui_renderer.md)), so both passes do walk the same HUD records. Measured with `src/carrier/hud_timing.cpp`: both queues are fully consumed between SubmitToGPU entry and pass-1's BeginSubmit Present and are FROZEN from `p1` through `b0` — the interpreter builds the 2D/HUD draw records once per frame BEFORE pass 1, and pass 2 re-walks the same record table. The HUD lands in both eyes' composites; the one-eye-HUD scenario is structurally impossible (the host quad-layer fallback is dead). Side finding — queue counter protocol: `+0x10` high16 = pending-unconsumed count cleared by the consumer pre-Present, low16 = ring position frozen during passes, `+0x14` unused. Residual HUD defects: [hud.md](hud.md).

## Known gaps (open, low priority)

- Shaders without `viewContextData` are not rewritten and lag the per-eye camera: explicit `g_ViewProjMtx` (same per-row `w` shift); `LocalToProj` (view folded in per object — needs the view-space eye offset, `clip.x -= P00*e.x`, P00 derivable from cached VP rows); `Mvp`/`TexGen`; rain. Check billboards/rain/particles/quads before implementing (not yet visibly wrong). Shader addresses: [shader_ctab_map.md](reverse_engineering/shader_ctab_map.md).
- PS-side camera data (`cameraPos` c92, texgen matrices) is mono — hook `SetPixelShaderConstantF` (slot 109) if reflections/shadows skew at IPD scale.
- Scene MSAA: [msaa.md](msaa.md).
