# GUI widgets and the GUI manager

The game's own widget layer on top of Scaleform ([gui_renderer.md](gui_renderer.md)): the HUD's images, text, minimap and flash movies. Scripts create and drive the widgets through `_GuiInternal` bindings ([lua_bindings.md](lua_bindings.md)). Per-address facts are on the Ghidra plates; names follow [ghidra-reva.md](ghidra-reva.md) § Naming.

## Classes

Each `Create*` binding constructs one class. The class names come from the bindings.

| Class | ctor | vtable | Binding |
|---|---|---|---|
| Widget (base) | `0x00626c40` | `0x00bbced8` | `CreateWidget` |
| ImageWidget | `0x0061ceb0` | `0x00bbc868` | `CreateImageWidget` |
| SpriteWidget (: ImageWidget) | `0x00621de0` | `0x00bbca58` | `CreateSpriteWidget` |
| TextWidget | `0x006222a0` | `0x00bbce58` | `CreateTextWidget` |
| MinimapWidget | `0x0061f640` | `0x00bbc958` | `MinimapCreate` |
| FlashWidget | `0x0061afc0` | `0x00bbc7e8` | `CreateFlashWidget` |
| MovieWidget | `0x00621ca0` | `0x00bbc9d8` | `CreateMovieWidget` |

**Slots:**
- `+0x04` Draw, `+0x08` per-frame Update.
- `+0x10` SetDesignRect, `+0x1c` GetDesignRect, `+0x20` pixel rect, `+0x24` SetRect.
- `+0x5c` screen-changed hook (TextWidget's caches the UI scale), `+0x70` `Widget_CorrectForResolution`.
- MinimapWidget adds `+0x7c` `MinimapWidget_DrawBorder`.

## Layout

- Widgets have a 640×480 design rect with left/right/centre and top/bottom anchors. `Widget_CorrectForResolution` maps it to pixels at scale H/480 (× `+0x2bd0` horizontally) from the view block ([view_blocks.md](view_blocks.md)). It caches the pixel rect (`+0x40..+0x4c`) and calls SetRect. `FlashWidget_SetRect` also sets the GFx movie viewport.
- **Re-layout triggers** (update thread):
  - a flash movie finishing loading (`FlashWidget_Update`);
  - a design-rect / anchor change;
  - Lua `CorrectWidgetForResolution`;
  - `GuiMgr_CheckViewCount`: the view-count cache `[mgr+0x60]+0` ≠ `shell+0x2b90` → slot `+0x5c` on every widget;
  - `GuiMgr_CheckAspectMode`: aspect mode `+0x2bc4` changed.
- **Per-frame view-scale readers** (not layout-cached): `MinimapWidget_UpdateBorderRect` (update thread) and the draw methods (build thread).

## GUI manager

- `mgr = g_GuiMgr` (`[0x01174fb0]`).
- `+0x60` screen info: `+0` view-count cache, `+4/+8` W/H floats, `+0xc` aspect mode. Written by `GuiMgr_OnScreenResize` (mutated entry; body `GuiMgr_OnScreenResize_Body`) at OnScreenResize, which also queues the Lua `NewScreenWidth/Height/...` event.
- `+0x68` widget table: `+0x44` array, `+0x4c` last index.
- `GuiMgr_Update` runs per frame on the update thread. `GuiMgr_DrawView` → `GuiMgr_DrawViewWidgets` draws one view's widgets on the build thread (`RenderShell_EndSubView` step 6).

## Minimap

- **Content:** the Scaleform movie in the minimap widget's rect. **Blips:** `MinimapWidget_Draw` → `MinimapWidget_DrawBlip`, sized by the live view scale.
- **Border:** `MinimapWidget_UpdateBorderRect` runs every frame on the update thread. rect = centre ± border size (`+0x1ac/+0x1b0`, 154 in gameplay) × view scale × `+0x2bd0` / 2, cached at `+0x1c0..+0x1cc`. `MinimapWidget_DrawBorder` draws it unchanged through `Gui_DrawTexturedRect`. Objective icons clamped to the edge use the same geometry.
  - Live 2026-11-09: 462 px at 2560×1440 (scale 3.0), 718.7 px at 2016×2240 (scale 4.667).
- `SetMinimapRadius` → `MinimapWidget_SetRadius`: the design rect = centre ± r. `SetMinimapBorder` stores the border fields, then calls a SecuROM VM routine (`0x006186a0` → `0x02566d60` → `0x01b950a0`) with the screen info.

## OPEN

- Image slot `+0x7c` (`0x0061ee70`), `ImageWidget_DrawTransformed`'s flag `+0x1b8`, and the base slots not listed above.
