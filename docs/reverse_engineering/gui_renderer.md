# GUI / HUD renderer (Scaleform GFx 2.x)

Found statically on 2026-11-08; per-address facts are on the Ghidra plates (`GuiRenderer_vtbl` `0x00bad178`). The HUD-placement problem is in [../hud.md](../hud.md); the record pipeline is in [scene_record_build.md](scene_record_build.md).

- **What draws the UI:** all UI (HUD, menus, pause screens) is Scaleform GFx 2.x Flash movies. They're driven from Lua (`mrxgui` module; `CreateFlashWidget`, `SetFlashSwfFile`, `SetFlashPauseMenu`, ...).
- **The renderer:** GFx draws through the engine's own `GRenderer` implementation, `GuiRenderer`. Its base vtable `0x00baceb8` is a destructor plus 29 purecalls. There's one instance, `g_GuiRenderer` `0x01174a58`, created by `GuiRenderer_ctor` `0x004adfe0` via `GuiRenderer_CreateInstance`. Each movie is registered with it by `GuiRenderer_RegisterMovie`.
- **Slot names:** inferred from the GFx 2.x interface order and confirmed by stack-argument sizes, e.g. `BeginDisplay` `ret 0x18` (6 args), `DrawIndexedTriList` `0x14`, `DrawBitmaps` `0x1c`.
- **UI = ordinary draw records.** The draw and mask slots emit `PgPrimitive` records through `PgPrimitiveScratchRecord_Submit`, inside `RenderShell_BuildSceneRecords`. There's no separate GUI draw path at the device level.
  - `DrawIndexedTriList` → `GuiRenderer_EmitTriListRecord` `0x004ae780`: type 8 (sort bucket 2), identity instance matrix, painter's-order depth key `1023/DAT_00d29e4c`. Gameplay HUD: ~39 records/frame (ret `0x004ae907`).
  - `DrawLineStrip` → `GuiRenderer_EmitLineRecord`, `DrawBitmaps` (text glyphs) → `GuiRenderer_EmitBitmapRecord`. `BeginDisplay`/`EndDisplay` and the mask trio emit records themselves.
  - A record is a GUI record iff its `Submit` caller lies in the renderer's code range, `0x004ae2d0`–`0x004b051b`.
- **Projection:** `BeginDisplay` builds the 2D ortho matrix from the movie frame rect (`renderer+0x6c1d4..0x6c1e8`, 2×3), composed with the user matrix. Aspect-mode-3 pillarbox uses the shell's screen fields. Per-draw GUI state (matrix, color transform, fill style) goes through thunk `0x004b0520` → runtime `0x024F0C70`, mutated native code traceable with `tools/securom_trace.py`. It writes the record command-stream slots (`0x00ff36c0`/`0x00ff36d4`). The live run confirms the transform travels in the command stream rather than `vsConstData` (below); the exact op is OPEN.
- **Why the first census missed it:** GFx batches heavily. The whole HUD is a few dozen records, and all of them land in sub-view 1 with the scene. Type 8 is also used by some scene emitters, so the record type alone doesn't identify GUI.

## Frame integration (live, 2026-11-08, `debug_build_census`)

```
RenderShell_BuildSceneRecords, per sub-view v:
  listener->vtbl[1](phase 2, v)  @0x0085acf6 (end-of-sub-view callback)
    -> RenderShell_EndSubView     (0x0062ee20, via a VM-slot jmp near 0x0062e62e)
         post-processing chain records (-> PgLtiBloom_EmitPasses)
         then GUI: GuiMgr_DrawView -> GuiMgr_DrawViewWidgets -> FlashWidget_Draw -> GFxMovieRoot_Display -> GuiRenderer_BeginDisplay ...
```

The stack chain is heuristic, but it agrees with the earlier `rt_probe` stack of the post chain.

- **Volume:** one `BeginDisplay` per frame in gameplay and pause, 0.7–1.2 in the front-end. All movies go through one display. GUI records per frame: front-end ~36–54, gameplay 44, pause ~84. Emission takes ~0.015–0.05 ms. The GUI is the last thing the build emits in its sub-view.
- **Draw order:** in the sorted list the GUI records are **contiguous**, after the scene and the post chain, i.e. drawn last. Exception: 3–6 records sort after them in gameplay (0 in menus), with sort keys above the GUI range. Their identity is OPEN; candidates are a screen fade/flash overlay or a final blit.
- **Record fields (gameplay):**
  - **View fields:** `viewIdx` 1, `screenIdx` 1, `stencilMode` 0.
  - **viewType:** 8 for draws, 5 for 2–3 setup records.
  - **rtWriteMask:** 02 for ~39 records, 01 for ~2–3.
  - **viewContextIdx:** the front-end uses 3, gameplay 12, pause 13. Gameplay's 12 is the world-camera record, so the per-eye camera upload ([view_rewrite.md](../view_rewrite.md)) also fires for HUD records. The GUI transform itself doesn't come from it (next point).
- **Transform:** every GUI record has a command stream and none has `vsConstData`, so the GUI matrix/color transform reaches the GPU through the record's command stream (RenderCmd ops). That fits the per-draw state routine `0x024F0C70` writing the command-stream slots.

## Draw time (live, 2026-11-09, `debug_gui_census`)

- **Scaleform is only part of the HUD.** In gameplay the ~44 GUI records were almost all the minimap:
  - setup: op 8 (SetScreenConstants) with the minimap rect (1369, 1568, 466, 466) at 2016×2240, then render-state ops and a stencil Clear (op 15) for a stencil mask;
  - draws: technique 127, ~40 records sharing one rotated 2D matrix.
- **The GUI transform:** each GUI draw uploads a 2×4 affine matrix to VS c0–c1 through op 2 (SetVSConstants) and a color to PS c0 through op 3 (SetPSConstants). The result is in pixels: the GUI shader then applies the shared pixel-to-clip VS c2 (skipping the records that upload c2 broke the minimap's scaling).
- **The engine's own 2D path:** the rest of the HUD (by elimination) draws as technique-73 records with no command stream, right before and right after the GUI block. They upload VS c2 = (2/W, −2/H, −1, 1), a pixel-to-clip mapping. Some depth-test against the scene depth with blending (world markers, presumably); some write depth.
- **Where:** everything draws into the front (`+0x3e98`) with the scene depth bound, in the final composite segment after the post chain. The records after the GUI are more technique-73 2D draws (view contexts 12/13), not a fade.
- **Frame end, in order:** the post chain; one draw back into main; the eye-adaptation luminance chain (64×64 → 1×1); the composite into the front (technique 78, whose stream binds the front and scene depth, then technique 79); the engine-2D HUD; the Scaleform block; a few more engine-2D draws.
- **Emitters** (build-time `Submit` return addresses, per-address facts on the Ghidra plates): most engine-2D records come from the renderer's vtable slot 11 (`0x00853390`). The technique-75 records come from slot 10 (`0x00853650`), which takes one depth value per batch. Some come from slot 17 of the vtable at `0x00bacde4` (`0x004ad0a0`). All are reached through vtables only, so the game code above them is what tells HUD from world marker.
- **Who draws what** (`RenderShell_EndSubView`'s order is on its Ghidra plate):
  - After the post chain and composite, `Hud_DrawWorldMarkers` (`0x0060f610`) draws most world icons: vehicle entry, pickups, objectives. Hiding its records removes them and nothing else. These are the depth-tested icons the in-game AA option breaks.
  - Steps 3–6 draw the rest of the HUD: `Hud_DrawOffscreenIndicators` (step 4), `Bink_DrawOverlays` (step 5), and `GuiMgr_DrawView` (step 6), which draws the game's widgets ([gui_widgets.md](gui_widgets.md)). Each widget draws through `Gui_DrawTexturedRect` / `TextWidget_DrawGlyphRuns`, or as a Scaleform movie, e.g. the minimap content.
  - The text records upload the shared pixel-to-clip `c2` that the Scaleform minimap also relies on.
  - Not mapped: destroyable-crate and helicopter-pickup icons, which come from neither `Hud_DrawWorldMarkers` nor the known widgets.
- **Identified by skipping records** (a one-off `debug_gui_skip` switch, since removed; in git history): the Scaleform block is the minimap content; the engine-2D view-type-8 records include the crosshair, the ammo indicator and the minimap outline. All engine-2D records share the same render states (depth test on, `LESSEQUAL`, blended), so render states can't separate HUD from world markers.

## Consequences for changing HUD rendering

- **Identification:** GUI records are identifiable at build time, exactly and cheaply: the `Submit` caller is in `0x004ae2d0`–`0x004b051b`, and the record index is the write count at `Submit` entry. They're also contiguous at draw time. That makes a record-level split possible: during the record walk, switch to a carrier-owned HUD render target at the first GUI record, draw the GUI once (not per eye), and composite it as its own layer. Per-eye placement then becomes a compositor problem instead of a constant rewrite.
- **Per-eye shift alternative ([../hud.md](../hud.md) fix direction 1):** rewrite the ortho translation in the GUI records' command-stream constant uploads per pass.

## OPEN

`debug_gui_census=on` logs the draw-time view of the HUD segment for these (`gui:` lines): record fields, build-time emitters, render commands, shader-constant uploads, draws.

- Where the destroyable-crate and helicopter-pickup icons are drawn: they land in the HUD layer, so in steps 3–6.
