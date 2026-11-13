# Sun shadows (cascaded shadow maps)

Game-side facts from a static read (2026-11-13). Per-address detail is in the Ghidra plates of the functions named here.

## Pipeline

- **Atlas:** `PgLtiShadow_CreateSurfaces` creates a 1024×4096 depth texture (`ShadowMapTex`, renderer+0x43f4) holding four 1024² cascades stacked vertically (viewport rects at renderer+0x4408..0x4444), plus a dummy R5G6B5 colour target. The format is `D24S8` when the byte at `[g_LtiRenderer]+0x5e8` is set (set by `FUN_00755570` from a format-cap flag 0x400), and `DF24` otherwise. Which one runs under DXVK is unchecked (OPEN, live). Fixed size: [reverse_engineering/rt_dims_source.md](reverse_engineering/rt_dims_source.md).
- **Fit:** `ViewContext_FitShadowCascades` (`0x00859ad0`) runs at the end of `ViewContext_BuildCameraConstants` (reimplemented, [culling.md](culling.md)). It is plain `.text`, not protected code.
  - The light view is a look-at from the camera position along the sun direction (shell+0x2bb0), up = world Y.
  - The base ortho is the tight light-space AABB of the whole shadow range, with z near pulled back 300 units for casters.
  - Each cascade is the tight x/y AABB of its slice's 8 corners. Outputs: the render VP (ctx+0xbf0), the scale/bias into its atlas quarter (ctx+0xdf0), and the world→atlas-UV matrix (ctx+0xcf0).
- **Splits:** base distance × 3ⁱ. The base is shell+0x2bc0 = 10.0 (constructor default; only Lua `SetShadowBaseDistance` writes it, live value unchecked). That gives 0–10, 10–40, 40–130 and 130–400, with a ~1% slice overlap. Shadows fade out over 350–400.
- **Caster pass:** depth bias 2⁻²⁰ and slope-scaled bias 2.5 (`RenderCmd_BeginSurfacePass`), cull NONE.
- **Receivers:** `GlobalLightData_Build` (`0x00465690`) uploads the fade (`globalLightData[9]`), the split ends ([10]) and the four world→atlas matrices ([11..26]). Sampler 15 (`RenderCmd_BindTextureAndSamplers`) is POINT/POINT, no mips, CLAMP or BORDER. The PS picks a cascade with a hard switch on view depth: no blend band between cascades.
  - **9-tap family** (315 shaders in `shader3.bin`): a 3×3 grid at texel offsets {−½, +½, +1½}, compared per tap (`z < sample`), then 2×2 box groups weighted bilinearly by `frc(uv·(1024,4096))`. Under point sampling this is a continuous 2-texel tent with a constant +½-texel shift. The texel size is hard-coded in the `def` constants (c0/c3/c4).
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

1. **Stabilize the fit:** implemented (2026-11-13) in `reimpl/render/fit_shadow_cascades.cpp`, conf `shadow_fit=stable|stock` (default `stable`). Not yet live-verified.
   - **Light view:** rotation only, aimed along the game's sun direction exactly. Its origin is snapped to a 256-unit grid near the camera.
   - **Cascades:** each is the bounding sphere of its slice. The centre lies on the slice axis, and the radius depends only on the slice's shape, so it doesn't change with rotation.
   - **Texels:** the size is T = 256 / N for an integer N, kept while it wastes at most 5%. The cascade centre is snapped to whole T. Since every origin step is a whole number of texels, the texel grid is fixed in the world.
   - **Depth:** from a bounding sphere of the whole shadow range, with the same 300-unit caster pull-back, so its scale doesn't change with rotation.
   - **Live checks:**
     - `shadowfit: selfcheck vs original: … bad=0`: the stock path matches the original bit for bit.
     - The `shadowfit: window:` line: `rescales=0`, and each cascade's texel min = max while the FOV holds (ADS zoom may rescale).
     - `sunDriftMax`: whether the sun moves at all.
     - In the HMD: shadow edges static under head motion. A/B with `shadow_fit=stock`.
   - **First live run** (2026-11-13, gameplay, HMD):
     - A significant visible improvement.
     - `selfcheck fits=240 bad=0`.
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
2. **Recover resolution:** retune the splits for the HMD (base distance and/or a practical-split scheme in `ViewContext_BuildCascadeFrusta`), and/or a 2048×8192 atlas. The atlas change touches the creation size, the cascade viewport table and the 9-tap shaders' texel `def` constants, which would be patched at `CreatePixelShader`.
3. **Filtering:**
   - Give the 1-tap (foliage/billboard) family the 9-tap kernel or better.
   - Optionally move every receiver to hardware PCF (`D24S8` + LINEAR, with the manual compare removed).
   - Add a blend band at cascade switches.

   All of these need replacement receiver shaders.

Live checks first: the atlas format (byte `[0x01175288]+0x5e8`), the live `ShadowBaseDistance` (shell+0x2bc0 = `0x017D16B0`), whether the sun direction changes per frame, and which materials use the 1-tap shaders.
