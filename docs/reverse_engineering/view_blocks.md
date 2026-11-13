# View blocks (per-sub-view screen geometry)

Who reads each sub-view's screen rect and UI scale, and on which thread. Used by the HUD layer's 16:9 layout ([../hud.md](../hud.md)). Per-address facts are on the Ghidra plates; every reader instruction has a `[viewblock scan 2026-11-09]` pre comment.

## Layout

- Block v is at `S + v*0xe80`, `S = [0x00dfb2f8]` (= `[0x00dfb2fc]` = `g_RenderShell` `0x017ceaf0`): `+0x20` x, `+0x24` y, `+0x28` W, `+0x2c` H (int32), `+0x30` byte flag, `+0x34` UI scale = H/480 (float).
- A view context is `block + 0x10` (vtable `ViewCtx_vtbl` `0x00bbc7e0`, destructor only). Its `+0x10..+0x24` are the same fields.
- Related shell fields: `+0x2b90` view count, `+0x2b92` current view index (word), `+0x2bd0` horizontal UI factor.
- **Only writer:** `RenderShell_SetupViewports` `0x0085a9b0`, called from OnScreenResize, SetupRenderTargets, `0x0085ab50` and `0x005e4dab`. It's reimplemented in the carrier (`reimpl/render/setup_viewports.cpp`).

## Readers (complete for direct dataflow: 69 instructions in 28 functions)

| Reader | Fields | Class | Thread |
|---|---|---|---|
| `Hud_DrawWorldMarkers`, `Hud_DrawPinnedMarkers` | scale, (pinned:) W/H | GUI, draws into the eye image | build |
| `Hud_DrawOffscreenIndicators`, `Hud_IndicatorNdcToPixels` | scale, W/H | GUI (HUD step 4) | build |
| `GuiRenderer_BeginDisplay` | x/y/W/H | GUI (Scaleform viewport) | build |
| `MinimapWidget_Draw`, `MinimapWidget_IsInMap` | scale | GUI | build |
| `MinimapWidget_WorldToMap` | scale | GUI | both |
| `MinimapWidget_UpdateBorderRect`, `TextWidget_OnScreenChange` | scale | GUI | update |
| `Widget_CorrectForResolution`, `ViewCtx_BuildSnapshot` (← `FlashWidget_SetRect`) | W/H, x/y/flag/scale | GUI | update |
| `GuiMgr_OnScreenResize_Body` | x/y/W/H | GUI | render, at resize |
| `LuaGui_FindGuiLocation` (view 0), `Reticle_UpdateState` (view 1) | x/y/W/H, W/H | GUI | update |
| `ViewCtx_GetViewport` `0x0085a7e0` | x/y/W/H, flag | per caller | — |
| `Scene_EmitViewPrimitives`, `FUN_00486460`, `FUN_0074efe0` (post chain) | W/H, x/y/W/H | scene | build |
| `Fov_TanIndexHelper`, `Camera_InitAspect` | W/H | scene (camera) | update |
| `LuaPlayer_GetViewport` (view 0) | x/y/W/H | script binding (`Player.GetViewport`) | update |
| `World_ProjectToViewPixels`, `FUN_0070cee0`, `FUN_0071c530` | x/y/W/H | unclassified | update / ? |

- `ViewCtx_GetViewport`'s callers: the scene (`ViewContext_BuildCameraConstants`, `FUN_00483070`, `FUN_00487070`, `FUN_0046d6c0`/`PgClouds_EmitCloudGen`/`FUN_0047f890`, `PgLtiBloom_EmitPasses`), and GUI code projecting world points into the eye image (`World_ProjectToPixels` → world markers, `LuaGui_FindGuiLocation`, the Bink 3D overlay; `Camera_BuildProjection` → the reticle).

## Consequences

- **The UI scale `+0x34` is GUI-only:** 9 readers, no scene/render code. Only world markers and pinned markers draw into the eye image.
- **x/y/W/H have scene readers on both threads**, including the camera (update thread). A temporary swap of the block races them.
- **World→eye-image projections must keep the eye size:** anything through `ViewCtx_GetViewport` / `World_ProjectToPixels`.

## Method and limits

- Two Ghidra dataflow scripts, union of results (`~/ghidra_scripts/ViewBlockRaw.py`, `ViewBlockReaders.py`; not in the repo). They follow S from `[0x00dfb2f8]`, `[0x00dfb2fc]` and the constant `0x017ceaf0` through registers, stack spills, call arguments and getter returns, plus the decompiler's dataflow.
- Block copies (`rep movsd`) all start at `+0x48` or later.
- **Not covered:** a block/ctx pointer stored in a heap object and read back later, unless the read ends in one of the functions above.
