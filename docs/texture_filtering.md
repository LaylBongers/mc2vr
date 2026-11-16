# Texture filtering

The game never uses anisotropic filtering ([reverse_engineering/texture_filtering.md](reverse_engineering/texture_filtering.md)). The mod adds it at the one function that picks every texture bind's filters: `RenderShell_ApplyViewSamplerStates`, reimplemented in `reimpl/render/sampler_states.cpp` ([reimpl.md](reimpl.md)).

- **Who gets it:** a linear-filtered bind of a 2D asset texture (`TextureView.type` 1) with more than one resident mip level, on a pixel sampler. MINFILTER becomes ANISOTROPIC and MAXANISOTROPY the configured level; MAG stays LINEAR and the mip filter stays the stock choice.
- **Who doesn't:** render targets and post-chain inputs (type 4), CPU-made lookup textures (type 0), cube maps (type 2, reflections), vertex samplers (257–260), point-filtered and single-level textures. They get exactly the stock states.
- **Setting:** `anisotropic_filtering = off | 2 | 4 | 8 | 16` (default 16), clamped once to `D3DCAPS9.MaxAnisotropy` and requiring `D3DPTFILTERCAPS_MINFANISOTROPIC`. `off` sends the device the stock call sequence.
- **Caches:** everything goes through `Dx9_SetSamplerState`, so `g_SamplerStateCache` stays in sync; a device Reset re-seeds device and cache to level 1 together.

## Verification

- 2026-11-16, non-HMD gameplay run at 16x (the device allows 16): a strong improvement, most visible on roads at grazing angles. The bounded first-bind-per-class log (`aniso: class …`) matched the rule. Mipmapped type-1 textures (e.g. 512² DXT1, 8 levels) got anisotropic filtering. Cubes, type 0, type 4, vertex samplers and single-level or point-filtered textures stayed stock. A second run also queried the device texture: every type-4 bind, including those whose stale record fields read "mipmapped DXT", is a single-level render target ([reverse_engineering/texture_filtering.md](reverse_engineering/texture_filtering.md)). So the rule leaves out no world textures. A `debug_gui_census` run then logged per-record binds. GUI textures were all single-level type-1 assets and stayed stock ([reverse_engineering/gui_textures.md](reverse_engineering/gui_textures.md)). Mipmapped world textures, DXT1/DXT5 from 16² to 1024² (3–9 levels, including clamped ones), got anisotropic filtering. Render targets and lookup textures stayed stock.
- Cost: not measured yet. A 16x / `off` A/B (2026-11-16, non-HMD 2560x1440, 120 Hz cap) compared only update-thread frame time (8.33 ms minus the pacer's `wait avg`): 3.48 vs 3.58 ms. That says nothing about anisotropic filtering, which is GPU work hidden behind the pacer's slack. Measure GPU time with `debug_gpu_census=on` (timestamp queries per pass and target).

## OPEN

- Not checked yet: in the HMD; `off` against stock (eye-pair dumps); GPU cost (`debug_gpu_census` A/B, desktop and HMD); the HUD layer, visually in the headset (its textures stay stock by the census).
