# GUI / HUD renderer (Scaleform GFx 2.x)

Found statically on 2026-11-08; per-address facts are on the Ghidra plates (`GuiRenderer_vtbl` `0x00bad178`). The HUD-placement problem is in [../hud.md](../hud.md); the record pipeline is in [scene_record_build.md](scene_record_build.md).

- **What draws the UI:** all UI (HUD, menus, pause screens) is Scaleform GFx 2.x Flash movies. They're driven from Lua (`mrxgui` module; `CreateFlashWidget`, `SetFlashSwfFile`, `SetFlashPauseMenu`, ...).
- **The renderer:** GFx draws through the engine's own `GRenderer` implementation, `GuiRenderer`. Its base vtable `0x00baceb8` is a destructor plus 29 purecalls. There's one instance, `g_GuiRenderer` `0x01174a58`, created by `GuiRenderer_ctor` `0x004adfe0` via `FUN_0060e780`. Each movie is registered with it by `FUN_0060e680`.
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
    -> FUN_0062ee20               (via a VM-slot jmp near 0x0062e62e)
         post-processing chain records (-> FUN_0074fae0)
         then GUI: FUN_00609d80 -> mrxgui manager -> GFx core FUN_007cafd0 -> GuiRenderer_BeginDisplay ...
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
- **The GUI transform:** each GUI draw uploads a 2×4 affine matrix to VS c0–c1 through op 2 (SetVSConstants) and a color to PS c0 through op 3 (SetPSConstants). Its translation (~16–18) is not in clip units, so the GUI shader probably applies a further screen mapping; not checked.
- **The engine's own 2D path:** the rest of the HUD (by elimination) draws as technique-73 records with no command stream, right before and right after the GUI block. They upload VS c2 = (2/W, −2/H, −1, 1), a pixel-to-clip mapping. Some depth-test against the scene depth with blending (world markers, presumably); some write depth.
- **Where:** everything draws into the front (`+0x3e98`) with the scene depth bound, in the final composite segment after the post chain. The records after the GUI are more technique-73 2D draws (view contexts 12/13), not a fade.
- **Frame end, in order:** the post chain; one draw back into main; the eye-adaptation luminance chain (64×64 → 1×1); the composite into the front (technique 78, whose stream binds the front and scene depth, then technique 79); the engine-2D HUD; the Scaleform block; a few more engine-2D draws.
- **Emitters** (build-time `Submit` return addresses, per-address facts on the Ghidra plates): most engine-2D records come from the renderer's vtable slot 11 (`0x00853390`). The technique-75 records come from slot 10 (`0x00853650`), which takes one depth value per batch. Some come from slot 17 of the vtable at `0x00bacde4` (`0x004ad0a0`). All are reached through vtables only, so the game code above them is what tells HUD from world marker.
- **Who draws what** (`FUN_0062ee20`'s order is on its Ghidra plate): after the post chain and composite, `Hud_DrawWorldMarkers` (`0x0060f610`) draws most world icons (vehicle entry, pickups, objectives; hiding its records removes them and nothing else). These are the depth-tested icons the in-game AA option breaks. The rest of the HUD follows in steps 3–6: `FUN_0061f430` draws the HUD icons and text, `FUN_00625620`'s records upload the shared pixel-to-clip `c2` that the Scaleform minimap also relies on, and the Scaleform block is the minimap content. Destroyable-crate and helicopter-pickup icons come from neither `Hud_DrawWorldMarkers` nor these; not mapped.
- **Identified by skipping records** (`debug_gui_skip`): the Scaleform block is the minimap content; the engine-2D view-type-8 records include the crosshair, the ammo indicator and the minimap outline. All engine-2D records share the same render states (depth test on, `LESSEQUAL`, blended), so render states can't separate HUD from world markers.

## Consequences for changing HUD rendering

- **Identification:** GUI records are identifiable at build time, exactly and cheaply: the `Submit` caller is in `0x004ae2d0`–`0x004b051b`, and the record index is the write count at `Submit` entry. They're also contiguous at draw time. That makes a record-level split possible: during the record walk, switch to a carrier-owned HUD render target at the first GUI record, draw the GUI once (not per eye), and composite it as its own layer. Per-eye placement then becomes a compositor problem instead of a constant rewrite.
- **Per-eye shift alternative ([../hud.md](../hud.md) fix direction 1):** rewrite the ortho translation in the GUI records' command-stream constant uploads per pass.

## OPEN

`debug_gui_census=on` logs the draw-time view of the GUI records for these (`gui:` lines).

- The full HUD segment before the GUI block (the first census logged only 8 records of it), and where the 2D path's VS c2 is first set.
- Whether the GUI shader (technique 127) also goes through c2.
- Where the destroyable-crate and helicopter-pickup icons are drawn, and which of steps 3–6 draws which HUD element.
