# Sky, haze and cloud layer

How the game draws the sky, the distance haze and the cloud layer, and how they meet the scene's depth. Per-address facts are on the Ghidra plates (`Sky_EmitRecords`, `PgClouds_EmitCloudRender1/2`). The clouds' constants and per-eye sky matrices: [../view_rewrite.md](../view_rewrite.md#clouds). The mod's fix (the sky emitter reimplemented, `reimpl/render/sky_emit.cpp`): [../clouds.md](../clouds.md).

## Records, in emission order

1. Cloud generation (`PgClouds_EmitCloudGen`) into the 512×512 `mCloudSurfRT`. Then cloud render 1 (`PgClouds_EmitCloudRender1`) draws the cloud plane, lit by `atmosphereData`, into the half-size `mCloudOutRT`, and a technique-less record rebinds main + scene depth with the full viewport, z 0..1.
2. `Sky_EmitRecords` (`0x0047f890`, vtable slot at `0x00bab634`). It first sets the scratch globals that force every following Submit into sort bucket 2 (`0x011755c1` = 1, `0x01163750` = 2), and sets the depth key `0x0116373c` to 1. Its records:
   - **A**, no draw. Op 8 sets the view's viewport with **MinZ = MaxZ = `(P22·d + P32) / (P23·d + P33)`**, `d` = 0.8 × the camera entry's far: the post-projection depth at 0.8 × far. Then op 11 (clip planes off). The water-reflection view takes its rect from the water object instead.
   - **B**, the sky dome (`PgSkyVP`/`PgSkyFP`, tech 86). It is a 16-unit sphere around the camera, so the forced depth alone places it at 0.8 × far. Its stream first copies main into `ColorBufferCopy` (op 12). Its PS blends `ColorBufferCopy` toward the sky's in-scatter colour by the linear depth read from `DepthBufferCopy` (`projectParams` = A, B, haze start 0.8 × far, 1 / (0.99 × far − start)). LESSEQUAL, no z-write: it covers the sky and everything beyond 0.8 × far, and hazes that into the sky.
   - **C**, the sun (tech 87).
   - When the sky parameters' flag is clear: the clouds object's slot 2, a tail jump into slot 6 = **cloud render 2** (`PgClouds_EmitCloudRender2`, tech 85). It draws the 7000-unit cloud plane into main, sampling `mCloudOutRT` at the screen uv, LESSEQUAL, no z-write, blended. Its sky matrix's depth terms (`A'` = 1.0000019, `B'` = −`A'`) never clip; the depth that is tested comes from the window. So the cloud layer covers only what the haze covers (the sky and terrain beyond 0.8 × far), and its opacity hides the distant mountains behind it.
   - **D**, the viewport-restore record: op 8 back to z 0..1, then op 10.

## The sort splits the window (engine bug)

A, B and C are type 6. Cloud render 2's material makes it type 8, and so is D. The sorted list puts all type-6 records before the type-8 ones, so between C and render 2 come any type-8 records with a lower key. Live (2026-11-16, HMD off, `debug_gui_census=clouds`):
- No water in view: A, B, C, render 2, D are contiguous, and render 2 draws with the window (`z[0.99985, 1.00085]`). MaxZ reads back as MinZ + 0.001, adjusted somewhere below op 8.
- Water in view: the water's depth pass (`FUN_00487ea0`, emitted just before the sky, type 8 with the same depth key 1) sorts between C and render 2. It binds `DepthBufferCopy` and then main, and each `SetRenderTarget` resets the D3D9 viewport. Render 2 then draws at z 0..1: the cloud plane is tested at its own depth (as if the near plane were 1) against the scene. It covers distant mountains wherever the plane is nearer, and pops as water enters and leaves the view.

`Dx9_SetRenderTarget` and the op-7 handler are the game's own, so this is an original-game bug. Confirmed by eye in the unmodified game (2026-11-16): moving the camera so that the water stops drawing makes the obscured distant mountains pop into view. The mod's fix (the sky emitter reimplemented, `reimpl/render/sky_emit.cpp`): [../clouds.md](../clouds.md).
