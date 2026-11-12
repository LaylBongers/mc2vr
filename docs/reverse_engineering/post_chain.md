# The post chain (`PgLtiBloom_EmitPasses`)

Static RE, 2026-11-12: from the decompile and disassembly, with shader facts from bytecode (`tools/shader_disasm.py`). Not checked live unless marked. Per-address facts are on the plates (`PgLtiBloom_EmitPasses`, `PostFxCompositeShader_ResolveConstants`, `EffectPass_ResolveConstantRegisters`, `PostFx_BuildMotionBlurMatrix`, `g_RenderCfg_MotionBlur`). Reimplementation: [../reimpl.md](../reimpl.md); mod change: [../motion_blur.md](../motion_blur.md).

## Records

`PgLtiBloom_EmitPasses` (bloom vtable `0x00bd4a14` slot 1, called by `PostFx_ChainAndComposite` on the producer side) emits the whole chain as scratch records, each a screen quad (`RenderShell_EmitScreenQuad`). In order:
1. **Bloom pyramid:** op 12 (main → ColorBufferCopy), then 4 downsample levels (/2, /4, /8, /16 of the viewport) with 0, 1, 2 and 4 two-pass blurs (`g_PostFxBlurPassA` into the level's temporary, `g_PostFxBlurPassB` back; which axis each blurs is not checked). A level's `blurScale` is (r/W, r/H, 1/W, 1/H), with r = 0.4 and 0.8 × `bloom+0x04`, then 0.45 and 0.95 (`g_Const_0_95`, a shared constant).
2. **Flares:** `FUN_0074efe0` draws into main (`+0x3e90`; probably the sun/lens flares, not read closely), then op 12 again if it drew any.
3. **Luminance and adaptation:** 64×64 → 16 → 4 → 1 (`g_PostFxLumPass` for 16 and 4, `g_PostFxLumFinalPass` for 1; `texSize` = 1/(4·size)), then the adaptation pass (`g_PostFxAdaptiveLumPass`). Op 22 (StretchRect) first copies last frame's result (`+0x22c4`) to `+0x22e4`. Its `adaptiveParams` = (clamp(g_Dt, 1/120, 0.1), `bloom+0x10`, `+0x14`, pow(`+0x10`, `+0x14`·dt)), `minmaxAdaptiveParams` = `bloom+0x18..+0x24`. Adaptation itself: [hdr_adaptation.md](hdr_adaptation.md).
4. **A half-size pass** into `+0x22c8` (material `+0x2080`), with no constants. Inferred to be the DOF blur image (`blurMap2`); the sampler binding is not checked.
5. **The composite** into the front (`+0x3e98`) with the auto depth (`+0x3e94`), drawn with `g_PostFxCompositeTechnique` (every other pass uses `g_PostFxQuadTechnique`).

The only condition is a sub-view count ≤ 2 at the top: no quality, MB or DOF switches. This corrects [../msaa.md](../msaa.md)'s earlier reading: the post chain's "one draw back into main" is the flare pass; the final image goes to the front.

## The composite (`shader3.bin 0x24e990`)

One full-screen draw does tone mapping with adaptation (`luminanceMap`), bloom (`blurMap`), film grain (`grainyMap`), grading (`monochromeMap`), depth of field (`blurMap2`, `depthMap.x`) and motion blur (`originalMap` taps, `depthMap.y`). So it can't be dropped. Its constants (registers on `g_PostFxCompositePass`, resolved by name; the stream op-3 order is image, filter, project, dof, X, Y, W, scale):
- `imageParams` = (`bloom+0x08`, `+0x0c`, (1 − s)·0.5 + s·1.5), with s = shell `+0x2bd8` (role inferred: saturation); `.w` unwritten.
- `filterParams` = (`bloom+0x28`, `+0x2c`, `+0x30`, 1 − the camera entry's `+0x6c`).
- `projectParams` = (P[2][2], P[3][2]) of the view's projection: linearizes the stored depth.
- `dofParams` = the camera entry's `+0x5c..+0x68`, with `.x` = 1/(y − x) and `.z` = 1/(z − w) (focus ranges).
- `MotionBlurConstantX/Y/W` = columns 0, 1, 3 of `PostFx_BuildMotionBlurMatrix`'s output (a pure function of the view context). `MotionBlurVelocityScale` = (`bloom+0x34`, 0, 0, 0).
- The grain transform is written per frame into the composite material (`bloom+0x1f58..+0x1f8c`): a random offset from the `g_PostFxGrainRng` LCG and a rotation by `+0x1f68` through `g_SineTable`.

Shader math:
- **Motion blur:** velocity = ((X·p, Y·p) − uv·(W·p)) × `depthMap.y` × scale, with p = (uv, stored depth, 1), sampled over 4 taps. A camera reprojection.
- **DOF:** sharp weight = max(`filterParams.w`, saturate(…)·saturate(…)); the rest is `blurMap2`. So the camera entry's `+0x6c` is a DOF strength (who sets it, e.g. cutscenes, is open).
- A second blur PS, `0x1fdad8`, takes the same four constants and masks on `depthMap.z` < 1/255. Its use is unknown; its resolver is probably `FUN_0046da50` (not checked).

## The `MotionBlur` setting

`Mercs2.ini [Render] MotionBlur` (default 1) is `g_RenderCfg_MotionBlur`, written by `RenderConfig_LoadIni` and the options menu (`LTIVideoSwitchOpt1`). It isn't an on/off for the blur:
- `SubmitToGPU` uploads it as the pass constant `blurAndTint` (0 for `stencilMode == 2` records).
- The depth prepass PSes write that into DepthBufferCopy's `.y`, a per-object mask.
- The alpha-tested prepass PS `0x2112f0` has no `blurAndTint` and writes depth there, so foliage blurs with the setting off.

## Effect pass constant names

`EffectPass_ResolveConstantRegisters` (`0x0085b50a`, entry `_Gate` `0x0085b500` through a call gate) fills every effect pass's register fields from the PS's constant names, the PS analog of `Technique_ResolveConstantRegisters`. The names, in pass order from `+0x94`, are `numPointLights`, `numSpotLights`, `pointLights`, `spotLights`, `globalLightData`, `materialData`, `cameraPos`, `user`, `viewport`, `depthXform` (`+0xdc`: what `SubmitToGPU` uploads the view record into, [view_context_records.md](view_context_records.md)), `dxt1NormalMap` and `blurAndTint`. Pass-specific resolvers call it first, then add their own (e.g. `PostFxCompositeShader_ResolveConstants`, `+0xfc..+0x134`). The field map: `game/pg_primitive.hpp`.

## Pass objects

Each pass shader is a static effect-pass object (constructors in `0x00a7xxxx`/`0x00a8xxxx`) whose class resolver is vtable slot 4: `BlurShader_ResolveConstants`, `LumDownsampleShader_ResolveConstants`, `AdaptiveLumShader_ResolveConstants` and `PostFxCompositeShader_ResolveConstants`. Each calls `EffectPass_ResolveConstantRegisters` first, then puts its own constants at `+0xfc` onward. The emitter reads those registers straight from the objects (`g_BlurPassA_BlurScale` …).

