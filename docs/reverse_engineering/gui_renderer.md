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

## Consequences for changing HUD rendering

- **Identification:** GUI records are identifiable at build time, exactly and cheaply: the `Submit` caller is in `0x004ae2d0`–`0x004b051b`, and the record index is the write count at `Submit` entry. They're also contiguous at draw time. That makes a record-level split possible: during the record walk, switch to a carrier-owned HUD render target at the first GUI record, draw the GUI once (not per eye), and composite it as its own layer. Per-eye placement then becomes a compositor problem instead of a constant rewrite.
- **Per-eye shift alternative ([../hud.md](../hud.md) fix direction 1):** rewrite the ortho translation in the GUI records' command-stream constant uploads per pass.

## OPEN

`debug_gui_census=on` logs the draw-time view of the GUI records for these (`gui:` lines).

- What the 3–6 records that draw after the GUI in gameplay are.
- The exact RenderCmd op/register that carries the GUI matrix in the command stream.
- Which render target the GUI draws into, and whether anything (post, AA resolve) runs after it.
