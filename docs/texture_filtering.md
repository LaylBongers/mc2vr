# Texture filtering

The game never uses anisotropic filtering ([reverse_engineering/texture_filtering.md](reverse_engineering/texture_filtering.md)). The mod adds it at the one function that picks every texture bind's filters: `RenderShell_ApplyViewSamplerStates`, reimplemented in `reimpl/render/sampler_states.cpp` ([reimpl.md](reimpl.md)).

- **Who gets it:** a linear-filtered bind of a 2D asset texture (`TextureView.type` 1) with more than one resident mip level, on a pixel sampler. MINFILTER becomes ANISOTROPIC and MAXANISOTROPY the configured level; MAG stays LINEAR and the mip filter stays the stock choice.
- **Who doesn't:** render targets and post-chain inputs (type 4), CPU-made lookup textures (type 0), cube maps (type 2, reflections), vertex samplers (257–260), point-filtered and single-level textures. They get exactly the stock states.
- **Setting:** `anisotropic_filtering = off | 2 | 4 | 8 | 16` (default 16), clamped once to `D3DCAPS9.MaxAnisotropy` and requiring `D3DPTFILTERCAPS_MINFANISOTROPIC`. `off` sends the device the stock call sequence.
- **Caches:** everything goes through `Dx9_SetSamplerState`, so `g_SamplerStateCache` stays in sync; a device Reset re-seeds device and cache to level 1 together.

## Verification

- 2026-11-16, non-HMD gameplay run at 16x (the device allows 16): a strong improvement, most visible on roads at grazing angles. The bounded first-bind-per-class log (`aniso: class …`) matched the rule. Mipmapped type-1 textures (e.g. 512² DXT1, 8 levels) got anisotropic filtering. Cubes, type 0, type 4, vertex samplers and single-level or point-filtered textures stayed stock. A second run also queried the device texture: every type-4 bind, including those whose stale record fields read "mipmapped DXT", is a single-level render target ([reverse_engineering/texture_filtering.md](reverse_engineering/texture_filtering.md)). So the rule leaves out no world textures.

## OPEN

- Not checked yet: in the HMD; `off` against stock (eye-pair dumps); frame-time cost at 90 Hz; the HUD layer, since the Scaleform GUI sets no sampler state of its own and inherits the last bind's.
