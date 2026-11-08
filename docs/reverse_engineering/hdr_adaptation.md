# HDR eye adaptation

The engine's auto-exposure state lives in GPU surfaces of the bloom object. It is not mirrored on the CPU.

- **Surfaces.** `PgLtiBloom_CreateSurfaces` (`0x0074dea0`, called by the bloom constructor `FUN_0074de20`, object 0x22f0 bytes, vtable `0x00bd4a14`) creates:
  - `_AdaptiveLuminanceSurface` (bloom `+0x22e4`) and `_pPrevAdaptiveLuminanceSurface` (`+0x22c4`): 1×1 fp16 RT textures;
  - `g_pAdaptiveDest` (`0x0117529c`): a 1×1 RT.

  At the creation log pushes the holder addresses sit in registers: `0x0074e83d` (ESI = `&adaptive`), `0x0074e9c3` (EDI = `&prev`).
- **Per frame** (commands built by `FUN_0074fae0`):
  1. render-command op `0x16` (= StretchRect, `0x00856c77`) copies prev → current;
  2. the adaptation draw writes the new value into prev, so **prev is the persistent value**;
  3. the timestep is `g_Dt` clamped to `[DAT_00beb198, DAT_00b92b58]`.

  In the two-pass stereo replay this runs once per pass.
- **On device Reset** the surfaces are re-created at their initial value. The first frame after a Reset then measures about zero luminance from the fresh chain and, with a reset-sized timestep, drives the adapted value to about 0 (observed 0.257 → 0.000), followed by a slow climb: a full-white frame, then a bright image adapting down. The mod's fix is in [../render_resolution.md](../render_resolution.md).
- `adaptiveParams` / `minmaxAdaptiveParams` are shader constants (`FUN_0046d0f0`); scripting exposes `SetAdaptiveLuminanceScale/Percent` and `SetTargetLuminance`.
