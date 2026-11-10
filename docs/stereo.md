# Stereo pipeline

GPU-boundary per-eye injection: the frame renders once per eye. During an eye pass the `viewContextData` uploads are rewritten for that eye ([view_rewrite.md](view_rewrite.md)); the eye images go to the [host](host.md) over [shared textures](shared_textures.md).

Premises (proven, do not re-litigate; RE detail: [draw_camera_chain.md](reverse_engineering/draw_camera_chain.md), [view_context_records.md](reverse_engineering/view_context_records.md)):

- The draw camera is external to the view system — it only crosses plaintext code as D3D constant uploads, so the GPU boundary (plus the camera table, [camera.md](camera.md)) are the injection points. Producer-side view duplication cannot work (E2: the ViewEntry pose fields are output channels).
- No fixed-function projection (`SetTransform` is never called); the projection is folded into the `viewContextData` VP rows, so per-eye asymmetric projection is an edit to the same rows.

## Second draw pass

Streams carry no draws, so each eye is a complete submission pass: the `PgPrimitive_SubmitToGPU` reimplementation runs its record walk twice, LEFT then RIGHT, with state and draws each time. The VCD uploads re-issue through the slot-94 rewrite, which keys on the pass. Code: `src/carrier/reimpl/render/submit_to_gpu.cpp` (the two passes), `src/carrier/vr/eye_replay.cpp` (the pass boundaries). Replay must use copied pointers (dedupe global `0x011697b8`). Pass 1 = LEFT, pass 2 = RIGHT; the replay happens after the frame's GPU-sync point (event-query spin in `LtiRenderer_BeginSubmit`) and costs ~3.2 ms.

## Outputs + capture points

The engine's final frame of each pass lands in the carrier-owned front ([render_outputs.md](render_outputs.md)). It is captured into the shared ring at 1→2 (LEFT) and 2→0 (RIGHT); the capture is also the MSAA resolve. Pass 2 simply re-renders into the engine's own targets, because pass 1 has already been composited and captured by then. The monitor shows the LEFT image through the carrier's mirror, with one Present per frame.

Lesson that still applies: never suppress backbuffer writes under SwapEffect=DISCARD (stale driver page).

## HUD/2D lands in both eyes

`g_RenderQueue2` (`0x00ff3650`, 2D/overlay) is consumed by unlocated code; the frame-ctx block carries `&g_RenderQueue2`. It is not thunk `0x0050c660` ([reverse_engineering/securom_vm.md](reverse_engineering/securom_vm.md)). The HUD/2D is ordinary draw records from the Scaleform renderer, built once per frame in the record build ([reverse_engineering/gui_renderer.md](reverse_engineering/gui_renderer.md)), so both passes do walk the same HUD records. Measured with `src/carrier/vr/hud_timing.cpp`: both queues are fully consumed between SubmitToGPU entry and pass-1's BeginSubmit Present and are FROZEN from `p1` through `b0` — the interpreter builds the 2D/HUD draw records once per frame BEFORE pass 1, and pass 2 re-walks the same record table. The HUD lands in both eyes' composites; the one-eye-HUD scenario is structurally impossible (the host quad-layer fallback is dead). Side finding — queue counter protocol: `+0x10` high16 = pending-unconsumed count cleared by the consumer pre-Present, low16 = ring position frozen during passes, `+0x14` unused. Residual HUD defects: [hud.md](hud.md).

## Known gaps (open, low priority)

- **Explicit camera matrices.** Shaders without `viewContextData` get the camera as matrices the game computes from the mono view-projection, uploaded by the record's command stream (op 2). `view_rewrite` corrects `LocalToProj` and `g_ViewProjMtx` with the pass's `D = VP_eye · VP_mono⁻¹` (`on_record_shader`, registers from the shader's CTAB, `vr/shader_constants`). In gameplay these are a projected ground-shade effect under vehicles (`LocalToProj` + `LocalToView` + `InvScrSize`, about 4 draws per frame) and one quad effect (`g_ViewProjMtx`). Uncorrected, the shades sat at the mono position, offset in one or both eyes. Corrected, they sit under the vehicles in both eyes. Not corrected:
  - `LocalToView`: needs the game's view-space axis convention.
  - `Mvp`/`TexGen` (2-row, screen-space): the Scaleform HUD, a separate track ([hud.md](hud.md)).
  - Rain/particles/billboards are unchecked. `debug_camera_census=on` lists every main-pass shader with camera constants outside `viewContextData` and who fills them. `debug_camera_census_skip=<name>` hides such draws, to identify an artifact.
- PS-side camera data (`cameraPos` c92, texgen matrices) is mono — hook `SetPixelShaderConstantF` (slot 109) if reflections/shadows skew at IPD scale.
- Water, from a static survey (2026-11-10, unverified live): the reflection pass (render size / 3, e.g. 853×480) uses a mono reflection camera that isn't rewritten, so both eyes see the same reflection. The water simulation's 128² height surface (created in VM code) may ping-pong; if so, the stereo frame steps it once per pass, twice per frame (water moving at double speed?). Check live before changing either.
- Scene MSAA: [msaa.md](msaa.md).
