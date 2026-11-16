# The direct-light clamp

Static RE, 2026-11-16. Prompted by a third-party RE note that PC Mercenaries 2 "clamps direct lighting to 1". Shader facts are from bytecode (`tools/shader_disasm.py`) and our verified ports ([../shaders.md](../shaders.md)).

## Where it is

- **Clamped in the originals:** every PgDiff family, in all 7 light variants, and PgFastFP: 287 shaders. `direct` is the sun (visibility · colour · N·L) plus SSS, rim and the local lights' diffuse. The clamp is per channel, before `materialData[0]` and the ambient add. In the sun-only original it is a single `mul_sat` (PgDiffSpecNormAmbOccFP, `shader3.bin@0xd9c70`).
- **Not clamped:** the terrain (`PgTerrainMesh*`), PgScrubLightFP, PgDecal2FP and PgDiffRefract*.
- **Not affected:** specular, reflections, emissive and ambient all add after the clamp.
- **Point-light attenuation** is also saturated (`saturate(dist·x + z)`), so a point light's colour constant is its peak radiance.
- **Ours** (both shadings) don't clamp the direct light. Only modern's rim/SSS `extra` term is still saturated.

## Why it matters: the frame is HDR up to the tonemap

- The main colour target is fp16 ([render_target_table.md](render_target_table.md)). The receivers clamp their output only at 30000.
- **The composite (`shader3.bin@0x24e990`, [post_chain.md](post_chain.md)) tonemaps locally:**
  - Ls = 0.8 · Y · e, with e = `imageParams.y` / (adapted luminance + 0.001);
  - Y_out = `imageParams.z` · Ls / (min(W, W^0.7, 10) + 0.7), where W is the blur target's alpha · e (a blurred luminance);
  - the result is clamped only at the end (`lrp_sat`/`mad_sat` with `filterParams`).

  So a lit pixel above 1 still maps to a brighter output, especially against a darker neighbourhood. The direct clamp, not the format, is what caps fire- and lamp-lit surfaces.
- **The blur targets are read through a packing, not linearly** (`blurMap` and `blurMap2`; bloom, local luminance in `.a`, DOF):
  - decoded = 2.6667 · clamp(x, 0, 0.375) + (255 · clamp(x − 0.375, 0, 0.625))^1.85;
  - 0..0.375 encodes 0..1 linearly;
  - above that, each 1/255 step is the next integer to the power 1.85, up to about 11,900;
  - in an 8-bit target nothing between 1 and 2 is representable.

  This is likely what the third-party note's "packed into 16-bit integers" describes. OPEN: the encode side and the blur targets' formats ([hdr_adaptation.md](hdr_adaptation.md) has the 1×1 luminance surfaces only).

## Light magnitudes (live)

Measured with `debug_light_census=on` (`debug/light_census.hpp`), 2026-11-16: ~2.5 min of daytime gameplay, classic shading, 18.8M lit-receiver draws. The light values come from the game's light context (`GlobalLightData_Build` copies the sun colour from `ctx+0x30`).

- **Sun:** above 1 on every draw but 332. The brightest channel is 1.10–1.29 and always red: (1.21, 1.17, 1.04), (1.17, 0.97, 0.83), (1.10, 0.92, 0.59) and 126 other colours as the day moves.
  - So the clamp engages on every sun-facing PgDiff/PgFastFP surface: wherever visibility · N·L > ~0.83, i.e. within ~34° of facing the sun.
  - Being per channel, it clips red first. Such surfaces go from the sun's warm colour towards neutral white, about 17% dimmer at most.
- **Point lights:** on 3% of draws (590k). There were no spot lights. Zero-colour entries (unused slots) make up 228k light-draws.
  - Of the non-zero light-draws, 74% are above 1. Peaks reach 5.94.
  - A fire/explosion family with colour ratio ≈ 1 : 0.61 : 0.37 (375 distinct scales, 171k light-draws) ranges 1.0–5.94; others reach 4.98.
  - Added to a sun above 1, a local light's clamped channels contribute nothing at all. At night a 5.9 fire would keep 17% of its red.
- **Ambient cube:** at most 0.54. It is not clamped.
- **`materialData[0]`** (applied after the clamp): mostly ≤ 1, some materials 2–3.
- Not measured: night (the sun's value then), and actual pixel coverage (the census sees constants, not N·L).

## Verdict

The third-party claim holds, and the clamp is likely a slip rather than a look:
- the sun is authored above 1, and fires and explosions far above it;
- the terrain, scrub and decal shaders light the same scene unclamped, so fires light the ground but not the PgDiff buildings and vehicles beside it;
- the composite tonemaps HDR.

**The mod:** our shaders drop the clamp in both shadings ([../shaders.md](../shaders.md#direct-light)). OPEN: a live look at classic, especially sun-facing walls and vehicles, and fires by day.
