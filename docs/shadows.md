# Sun shadows (cascaded shadow maps)

Game-side facts from a static read (2026-11-13). Per-address detail is in the Ghidra plates of the functions named here.

## Pipeline

- **Atlas:** `PgLtiShadow_CreateSurfaces` creates a 1024×4096 depth texture (`ShadowMapTex`, renderer+0x43f4) holding four 1024² cascades stacked vertically (viewport rects at renderer+0x4408..0x4444), plus a dummy R5G6B5 colour target. The format is `D24S8` when the byte at `[g_LtiRenderer]+0x5e8` is set (set by `FUN_00755570` from a format-cap flag 0x400), and `DF24` otherwise. Under DXVK it is **D24S8** (live, 2026-11-14: `shadow: atlas 1024x4096: depth D24S8`), the hardware-shadow-map path. Fixed size: [reverse_engineering/rt_dims_source.md](reverse_engineering/rt_dims_source.md).
- **Fit:** `ViewContext_FitShadowCascades` (`0x00859ad0`) runs at the end of `ViewContext_BuildCameraConstants` (reimplemented, [culling.md](culling.md)). It is plain `.text`, not protected code.
  - The light view is a look-at from the camera position along the sun direction (shell+0x2bb0), up = world Y.
  - The base ortho is the tight light-space AABB of the whole shadow range, with z near pulled back 300 units for casters.
  - Each cascade is the tight x/y AABB of its slice's 8 corners. Outputs: the render VP (ctx+0xbf0), the scale/bias into its atlas quarter (ctx+0xdf0), and the world→atlas-UV matrix (ctx+0xcf0).
- **Splits:** base distance × 3ⁱ. The base is shell+0x2bc0 = 10.0 (constructor default; only Lua `SetShadowBaseDistance` writes it, live value unchecked). That gives 0–10, 10–40, 40–130 and 130–400, with a ~1% slice overlap. Shadows fade out over 350–400.
- **Caster pass:** depth bias 2⁻²⁰ and slope-scaled bias 2.5 (`RenderCmd_BeginSurfacePass`), cull NONE.
- **Receivers:** `GlobalLightData_Build` (`0x00465690`) uploads the fade (`globalLightData[9]`), the split ends ([10]) and the four world→atlas matrices ([11..26]). Sampler 15 (`RenderCmd_BindTextureAndSamplers`) is POINT/POINT, no mips, CLAMP or BORDER. The PS picks a cascade with a hard switch on view depth: no blend band between cascades.
  - **9-tap family** (315 shaders in `shader3.bin`): a 3×3 grid at texel offsets {−½, +½, +1½}, compared per tap (`z < sample`), then 2×2 box groups weighted bilinearly by `frc(uv·(1024,4096))`. Under point sampling this is a continuous 2-texel tent with a constant +½-texel shift. The texel size is hard-coded in the `def` constants (c0/c3/c4).
  - **1-tap family, identified live** (2026-11-13; high-quality shaders only, as the live ini uses). Hashes are bytecode FNV-1a (`tools/shader_hash.py`); found with `debug_ps_census_watch` and the cycling `debug_camera_census_skip=ps:` groups. 8 of the 27 single-tap shaders in `shader3.bin` draw:
    - **PgFastFP** (`3e76835c`, `shader3.bin@0x1d6370`): the cheap generic material, covering vegetation and other "fast" meshes, including wind-animated ones. ~350 draws per eye per frame, ~1–2 Mpix. The lighting is identical to the 9-tap `PgDiffAmbOccFP` (`0x11570`) minus the filtering.
    - **PgScrubLightFP** (`765f3420`, `@0x2004c0`): scrub/brush vegetation. ~120 draws. The same code as PgFastFP, with vertex colour and different inputs.
    - **PgBillboardTreeFP** / `_fade` (`eacf64f8`, `@0x2ff0` and `@0x3790`, one bytecode): billboard trees, ~85 draws. Not noticed when hidden: probably far tree impostors.
    - **PgWaterFPDWE** (`c2dde179`, `@0x254a80`): water. ~2 draws, but **~0.8 ms GPU** with reflections off (OPEN: why it costs that much).
    - **PgDecal2FP** (`deef11a9`, `@0xa2a0`) and **PgDecal2FP_pl** (`8360631b`): the decals. They drew only in bursts at certain places (indoors at the start, one outdoor spot), a few draws per frame, and weren't visible when blinked at 1 Hz.
    - Names: [reverse_engineering/shader_containers.md](reverse_engineering/shader_containers.md). The 27 single-tap shaders are PgFastFP ×7, PgScrubLightFP ×7, PgDecal2FP ×7, PgBillboardTreeFP, and the water: PgWaterFP, PgWaterFP_LI, PgWaterFPDWE, PgWaterFPbl, PgUnderwaterFP. They match Mercenaries2Remastered's list (grass, bushes, trees, decals, water) exactly.
  - **1-tap family** (28 shaders in `shader3.bin` + every shader in `shader3Low.bin`): one `texldp` with a binary compare, so there is no filtering at all. These are the `diffuseMap`-only shaders without `materialData` (most likely foliage, per the Mercenaries2Remastered note) and the billboard trees (`gDiffuseTexture`/`gNormalmap`, beside the `gBillboardData` VS). The water shaders are also 1-tap. Material ownership is unconfirmed live (OPEN: `debug_ps_census` + `tools/shader_hash.py`).
  - The per-tap compare assumes the texture returns depth (`DF24`). With `D24S8` the hardware returns the compare result, which this shader thresholds again. That is still correct under POINT, but it rules out simply switching the sampler to LINEAR for hardware PCF.

The sun direction comes from the environment region the camera is in (shell+0x2bb0, written each frame by `FUN_00466be0`). Crossing a region boundary blends it over several seconds.

## Why edges shimmer (cause)

The fit has none of the usual stabilization: no bounding sphere, no extent quantization, no texel snapping.
- **Translation:** the light-view origin is the camera position, so the texel grid slides by sub-texel amounts with any camera move.
- **Rotation:** the tight AABB changes size with camera rotation, so the texel scale changes every frame.

Every shadow edge resamples each frame, which reads as crawling/flickering. On a monitor this happens only while the camera moves.

VR makes it continuous and larger:
- Head tracking never holds still, so the fit changes every frame.
- The fit uses the widened HMD-union frustum ([culling.md](culling.md)). Estimates with the sun at ~53° elevation, yaw swept, pitch ±20°: stock texels per cascade are about 1.0–1.7 / 3–7 / 9–22 / 27–66 cm. With the union frustum they are 1–4 / 3–16 / 9–53 / 27–163 cm. That is up to ~2.5× coarser, and the size swings 2–4× with head rotation (stock ~1.5–2×).
- Both eyes use the same matrices (built once per frame), so there is no inter-eye mismatch beyond cascade selection by each eye's own depth.

## OPEN: fixes (planned order)

1. **Stabilize the fit:** done (2026-11-13) in `reimpl/render/fit_shadow_cascades.cpp`. Always on: the `shadow_fit=stable|stock` A/B and the stock transcription were removed on 2026-11-14 after the runs below.
   - **Light view:** rotation only, aimed along the game's sun direction exactly. Its origin is snapped to a 256-unit grid near the camera.
   - **Cascades:** each is the bounding sphere of its slice. The centre lies on the slice axis, and the radius depends only on the slice's shape, so it doesn't change with rotation.
   - **Texels:** the size is T = 256 / N for an integer N, kept while it wastes at most 5%. The cascade centre is snapped to whole T. Since every origin step is a whole number of texels, the texel grid is fixed in the world.
   - **Depth:** from a bounding sphere of the whole shadow range, with the same 300-unit caster pull-back, so its scale doesn't change with rotation.
   - **Live checks:**
     - The `shadowfit: window:` line: `rescales=0`, and each cascade's texel min = max while the FOV holds (ADS zoom may rescale).
     - `sunDriftMax`: whether the sun moves at all.
     - In the HMD: shadow edges static under head motion.
   - **First live run** (2026-11-13, gameplay, HMD):
     - A significant visible improvement.
     - `selfcheck fits=240 bad=0`: the stock transcription (since removed) matched the original bit for bit.
     - Four fits per frame (~360/s).
     - OPEN: `rescales` were 7–25 per 10 s, cause unknown. The window line now breaks fits down per view (`shadowfit:   view` lines: grow/shrink, near plane and tanH ranges), because that run's texel ranges mixed the views.
     - The sun moved: two one-off jumps (65°, 49°) and a ~30 s stretch of 0.1–0.16° steps per fit. Cause, observed in game: **environment regions**. Crossing into one blends the sun direction, along with the lighting, the clouds and possibly the tone mapping; the shadows visibly move. The first build re-aimed the light only after 0.1° of drift, which turned a blend into steps every few frames. It now tracks the sun exactly: a still sun gives bit-identical matrices (the calm windows had `sunDriftMax=0.000°`), and a blend moves the shadows smoothly. Texel crawl is now confined to the blends, as the game intends the shadows to move then.
   - **Second live run** (2026-11-13; started indoors, went outdoors, then drove across the map). Gameplay began at 13:59:36.
     - **The two HMD views** (main `017cf980` and its copy `064e9d20`, both widened):
       - Constant texels, 4.56 / 16.94 / 54.94 / 169.54 cm.
       - They rescale only when the camera's near plane switches (0.2 / 0.3 / 1.0: camera modes, vehicles), once per switch.
     - **The narrow-FOV view** (`064eafb0`, most likely the water reflection) rescales often: its tanH varies 0.34–0.39. Water reflections are disabled in the live game settings (since the start of the mod), yet this view is still built every frame, so nothing shown uses its shadows today. OPEN for when reflections are enabled (planned): check whether its shadow fit is consumed, and if it hops visibly, widen its slack or fit it to a fixed FOV.
     - **The sun:**
       - Jumps of 65° and 49° at load-in.
       - It changed on every frame (0.03–0.09° per fit) for ~85 s during the indoor→outdoor stretch.
       - It was perfectly still for the whole 2.5-minute drive.
     - OPEN: whether those 85 s were a blend or jitter. `sunPath`/`sunNet` per window settle it: path ≈ net is a blend; path ≫ net is jitter, which would want an angular snap.
   - **Cost:** a sphere wastes texels against the tight box, which loses resolution near the camera (estimate, cascade 0: ~1.7 cm stock FOV, ~4 cm VR union). Step 2 recovers it.
2. **Resolution:** implemented (2026-11-14), conf `shadow_resolution=1024|2048|4096`, the edge of each cascade. The default is **2048**; 1024 is stock.
   - **Live, 2048 (2026-11-14):** clearly sharper, no artifacts. The atlas costs ~0.68 ms GPU per frame (both eye passes) against ~0.54 ms at 1024. 4096 was judged not worth it.
   - **What remains is the cascade transition**, mainly c0 → c1. Each cascade's texel size and the 9-tap filter's width jump about 4× at its boundary. The fix is a blend band in the receivers.
   - **Blend band, live (2026-11-15):** a significant visual improvement and comfort fix in the HMD. All 48 receivers substituted at the 2048 atlas, no errors. In `reimpl/shaders/shadow.hlsli` (`sun_shadow`): over the last 20% of cascades 0–2's depth range (`kCascadeBlendBand`), a second 9-tap lookup in the next cascade fades in linearly, reaching it at the boundary. With the stock splits the bands are 8–10 m, 34–40 m and 112–130 m.
     - The second lookup sits behind a dynamic branch, so only pixels in a band pay. It reads with explicit level-0 taps (texldl), since gradients aren't available inside the branch; the atlas has one level and is point-sampled.
     - The stabilized fit fits cascades 1–3 over their slice extended back over the previous cascade's band, with a 10% margin. The fraction appears in both places, so change both.
     - **Coverage:** every shadow receiver in `shader3.bin` has the band (2026-11-15). That's all 342 distinct receivers: the 27 single-tap ones, the 280 PgDiff materials and the 35 terrain shaders.
     - **Checks:** `shader_iface.py --shadow-band` accepts exactly one extra leaf `ifc…endif`. `shader_equiv.py --shadow lit` mocks the atlas fully lit or fully shadowed, so every cascade agrees and the band can't change a result; the rest is compared exactly (a negated input is still caught). Without these options both tools flag the band as a difference, as intended.
   - **Split retuning was dropped.** With the stabilized fit and the HMD FOV, a cascade's texel size is set by its far distance (~0.41 cm per metre). For a 10 m first split, geometric spacing over 400 m gives ends of 10/34/117/400, and the stock 10/40/130/400 is already that. Other splits only trade one distance band against another.
   - **Estimated texels per cascade, HMD union FOV:**

     | Resolution | c0 | c1 | c2 | c3 | Depth atlas |
     |---|---|---|---|---|---|
     | 1024 | 4.6 cm | 17 cm | 55 cm | 167 cm | 16 MB |
     | 2048 | 2.3 cm | 8.5 cm | 27 cm | 84 cm | 64 MB |
     | 4096 | 1.2 cm | 4.2 cm | 14 cm | 42 cm | 256 MB |

     The colour RT0 adds half as much again. The cost is mostly shadow-pass fill rate.
   - **What depends on the size** (survey 2026-11-14):
     - `PgLtiShadow_CreateSurfaces` creates the surfaces (push immediates) and the cascade table, which holds RECTs (left, top, right, bottom). Reimplemented, with the size from the conf: [reimpl/render/shadow_surfaces.hpp](../src/carrier/reimpl/render/shadow_surfaces.hpp).
     - `RenderCmd_BeginSurfacePass` hard-codes the 1024 viewport. Reimplemented and called directly from our `RenderCmd_ExecuteStream`, its only caller.
     - The 315 9-tap receivers and our 27 hold the texel size as `def` constants: ±0.5/1024, ±1.5/1024, ±0.5/4096, ±1.5/4096, 1024, 4096. No other shader uses those values. They are rescaled at `CreatePixelShader` by exact float bits, only in shaders with both half-texel signatures ([shaders.md](shaders.md)). An offline simulation at 2× and 4× changes exactly the 315 + 27 shaders.
     - Our stabilized fit takes its texel count from the conf.
   - **Safe at any size:** the depth bias (constant in depth units; the slope bias scales with the texel); `ShadowMapCombined` (never used after creation, so kept at the stock size); our view rewrite, MSAA and output code (keyed on the render size, not the atlas). The debug censuses' "atlas" category now follows the size.
   - **Limits:** DXVK's maximum texture size is 16384, so 4096 is the top setting. Nothing in the engine validates the size. A failed creation is logged as `shadow: ERROR` (the original would crash calling `GetSurfaceLevel` on it), and you need to pick a lower resolution.
   - **Live, 1024 (2026-11-14):** the reimplemented path behaves as stock: no visible change, `texelScaled=0`, no errors.
   - **OPEN, optimization:** the atlas is rendered in **both** eye passes, 1027 draws each (`debug_gpu_census`: 0.37 ms GPU in pass 1, 0.17 ms in pass 2, plus ~0.3 ms CPU each). It is eye-invariant (one fit per frame), so pass 2 could reuse pass 1's. The cost grows with `shadow_resolution`.
   - **Checks:** `shadow: atlas WxH: depth D24S8|DF24|surface` at device creation; `texelScaled=` in the `shaders: window:` line (≈340 at 2048/4096, 0 at 1024); shadows still in place at every setting, sharper at higher ones. Cost: `debug_gpu_census` per-pass times, 1024 against 2048 against 4096.

3. **Filtering** ([shaders.md](shaders.md)):
   - Done: all 27 single-tap (foliage, billboard, decal, water) receivers take the 9-tap kernel (2026-11-14), and every receiver has the blend band (step 2) and footprint-adaptive PCF (step 4).
   - Optional, not done: hardware PCF (`D24S8` + LINEAR, with the manual compare removed). It needs every receiver's per-tap compare rewritten.

4. **Footprint-adaptive PCF** (2026-11-16, `shadow.hlsli`, every receiver): percentage-closer filtering whose kernel follows each pixel's footprint in the shadow map, in size and shape, sampled N-rooks (one tap per row and per column). In effect anisotropic filtering for shadow maps.
   - **Problem:** thin shadows (fences, poles, trunks) flicker on surfaces seen at grazing angles, above all roads. There a screen pixel covers many shadow texels, but the game's tent samples only ~2 texels around the pixel centre. Head motion slides that sample across the thin shadow, so it is hit, then missed. Shadow maps can't be mipmapped, so this is the shadow counterpart of texture minification. It isn't caster-side: with the stabilized fit, a static caster rasterizes the same every frame.
   - **Fix:** the pixel's footprint in the atlas comes from the world position's derivatives through the cascade's rows (continuous across cascade switches, and usable for the band's second lookup without derivatives inside its branch). Up to 2 texels per pixel, the filter is exactly the game's tent. From 2 to 4 texels the 9 taps move to an even spread over the footprint ("N-rooks": one tap per row and per column), each weighted 1/9. A thin shadow then moves the result by about 1/9 instead of all the way. Footprints are capped at 32 texels, and taps are clamped to their cascade's atlas quarter. It adds no taps and no branches, only ALU.
   - **Checks:** `shader_iface.py --shadow-band` and `shader_equiv.py --shadow lit` pass on all 342 receivers. `shader_equiv.py --derivs zero` (zero footprint, per-texel shadow noise) matched the pre-filter shaders on all 342 with the quarter clamp disabled. The clamp is the only deliberate difference at small footprints. The new constants are resolution-independent, so the `shadow_resolution` rescale needs no change: the clamp inset is a fixed 2⁻¹⁵ rather than half a texel, which would have folded into a constant the rescale doesn't know.
   - **Not addressed:** casters thinner than a texel that move (wind-animated foliage, swaying antennas), which pick up different texels every frame. Only resolution helps there.
   - **Live (2026-11-16):** a big improvement to the remaining flicker comfort issues, judged by eye in the headset. A non-HMD run the same day was clean: all receivers substituted, `texelScaled=362` at 2048, no errors.
   - OPEN: GPU cost (`debug_gpu_census`, main-scene segment). It adds ALU only, no taps.

Of the early live checks, the atlas format (D24S8), the sun direction's behaviour (environment-region blends) and the 1-tap materials are settled above. Still unchecked: the live `ShadowBaseDistance` (shell+0x2bc0 = `0x017D16B0`; the splits above assume the default 10.0).
