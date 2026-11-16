# GUI textures (Scaleform)

Found statically on 2026-11-16. Per-address facts are on the Ghidra plates: `GuiTexture_vtbl` `0x00bacf40` (slot map) and its methods `GuiTexture_*` / `GuiTextureBase_*`, `GuiRenderer_TrackTexture` / `_ReleaseAllTextures`, `GImageInfo_*`, `GuiImageCreator_CreateImage`, `TextureRegistry_FindOrCreateAddRef` / `_Release`, `Resource_AddRef`, and the bitmap-fill setup `FUN_004aeca0`. The renderer side is in [gui_renderer.md](gui_renderer.md).

- **Scaleform owns no pixels.** The game's `GTexture` implementation (`GuiTexture`) has no D3D texture. `GuiTexture_InitTexture` hashes the image's *name* and requests that name as an ordinary game texture: `TextureRegistry_FindOrCreateAddRef({hash, 0xf011157a}, ref class 3)`. The texture is the same file-loaded asset entry (kind 2, `TextureView.type` 1) that world textures use, shared with any other user of that name. The asset loader fills its levels, format and mip chain from the texture file.
- **Images resolve by name.** `GuiImageCreator_CreateImage` (vtable `0x00bbc0ec`, installed as GFx state 0xc by `GFxState_ctor`) handles only file images: it makes a pixel-less `GImage` carrying the file name without its extension. Bitmaps embedded in the SWF are rejected (NULL). External images, gfxexport font textures and `img://` loads all reach a game asset this way. The movies themselves are in-memory resource entries too: tag `0xfe0e8320` in the same registry (`FUN_0060d8c0`).
- **Lifetime.** `GImageInfo_GetTexture` creates the `GuiTexture` (`GuiRenderer_CreateTexture` → `GuiTexture_ctor`) and initializes it through `GImageInfo_Recreate`. The renderer tracks it (`GuiRenderer_TrackTexture`); `GuiTexture_ReleaseResources` drops the registry reference and tells the change handlers (event 2 → `GImageInfo_OnChange`).
- **No dynamic textures.** `InitDynamicTexture` and `Update` are stubs, so there is no GFx glyph cache. Text uses pre-rendered gfxexport font textures.
- **Sampler state is per draw, not per asset.** The bitmap-fill setup (`FUN_004aeca0`) rewrites the shared entry's `samplerFlags` on every draw from the fill's wrap and sample modes. That also clears `0x40`, so GUI mip filtering is always linear. The UV transform uses the movie's declared image size (`GuiTexture+0x28/+0x2c`), not the asset's.
- **`g_TextureRegistry` is a general resource registry.** Entries are keyed {name hash, type tag}; textures are `0xf011157a`, movies `0xfe0e8320`. The global keeps its older name because code and docs cite it.

## Consequence for texture filtering

A GUI texture's `TextureView` doesn't say it's a GUI texture: it's an asset record like any other. Our anisotropic-filtering rule ([../texture_filtering.md](../texture_filtering.md)) reaches GUI textures only through their level count. UI art authored without mips stays stock; a mipmapped one would get anisotropic filtering, which is invisible on screen-aligned quads. The one place that knows a bind is GUI is the bitmap-fill path, which owns the flags per draw.

Live (2026-11-16, `debug_gui_census=on`, 3 captures, 184 GUI records): the GUI path bound 14 textures. All were type-1 assets with a single mip level (DXT5, 16x16 to 512x512), with flags 0 or 6 as the bitmap-fill setup writes them, and all stayed stock. Most GUI records draw untextured tri lists or reuse the bound texture, which the bind cache skips.

## Naming notes

- The trivial vtable stubs (return-true, no-op, field getters) are shared by up to 121 vtables and stay unnamed. Some Scaleform library exports were attached by identical-code folding to unrelated code, so their names are wrong at those sites (e.g. `ChangeSize`, a `GZLibFile` export, also sits in the `GImageInfo` handler vtables). The Ghidra function for `GuiTexture_ctor` covers only its first 24 bytes; the plate gives the real extent.

## OPEN

- The game's own widgets: `Gui_DrawTexturedRect` (`0x006178c0`, a mutated stub; body at runtime `0x00412d97`) and `TextWidget_DrawGlyphRuns` pick materials by hash, and bind textures inside VM thunk `0x00854740`. Their sampler flags are presumably the assets' own; unverified.
- Slot `+4` (`InitDynamicTexture`) takes 6 arguments where GFx 2.x has 5, so the slot assignment there is uncertain.
