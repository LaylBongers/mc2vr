# Scene MSAA

OPEN track: add working MSAA for the 3D scene.

## Problem

The PC port's MSAA option has no effect on the 3D scene. This is a known port bug; forcing MSAA in the driver reportedly works. The engine's HDR scene renders into the fp16 main texture (`g_RT_MainColorSurf`, `+0x3e90`), and D3D9 textures cannot be multisampled. The MSAA setting (`g_LtiRenderer+0x594`, the sample count) only reaches:
- the swapchain backbuffer and its auto depth, and therefore only the composite and HUD drawn there (render-command op 6). With it on, depth-tested composited icons (world markers) fail to draw — reproducible without the mod. The carrier ignores the request ([render_outputs.md](render_outputs.md)): at HMD size the 8× composite + resolve cost ~0.5 ms/frame for HUD edges only;
- the choice of a separate non-MSAA scene depth (`+0x3ea0`).

RE: [reverse_engineering/render_target_table.md](reverse_engineering/render_target_table.md).

## Approach (to be designed)

Render the main scene pass into a carrier-owned **MSAA render-target surface + MSAA depth** in place of `+0x3e90` / `+0x3ea0`. Then **resolve** it (StretchRect) into the main texture's level 0 before anything samples the main texture.

RE needed first:
- **Where post-processing first reads the main texture** (sampler bind or StretchRect source) — the resolve point. It is per pass in the stereo replay.
- **The depth copy `+0x3e9c`** (full-size, fmt 115) and every other reader of the scene depth. An MSAA depth cannot be sampled, so depth consumers need a resolved or non-MSAA copy. Check whether the depth copy is written by a scene MRT (`+0x5e4` MRT mode bit).
- **What binds `+0x3e90` / `+0x3ea0`:** `LtiRenderer_BeginSubmit`, render-command target switch case 7, the clouds code. The MSAA pair must replace them for the scene draws only.
- **Cost:** sample count vs the native render size ([render_resolution.md](render_resolution.md)).
