# Render surface census & dimension sources — OPEN

Pre-work for [../render_resolution.md](../render_resolution.md): enumerate the engine's render surfaces and map where sizes come from, to decide (a) which creation calls the size-substitution hooks rewrite, and (b) whether engine CPU-side sizes self-adapt (desc-driven) or need rescale hooks (config-driven).

## Questions / methods

- **Q1 — surface census.** Instrument `CreateTexture` (slot 23) / `CreateRenderTarget` (28) / `CreateDepthStencilSurface` (29) with size+format+usage+caller logging across gameplay AND menus → the chain map: which surfaces are backbuffer-derived (exact 2560×1440? its fractions?) vs independently sized (shadow maps, light scopes). Output = the substitution keying rules.
- **Q2 — CPU-side dims.** After a substituted (native) surface exists, does the engine set viewport/scissor/HUD layout from `GetDesc`/`GetViewport` readback (self-adapts — no hook needed) or from the config value that requested the size (needs `SetViewport`/`SetScissorRect` rescale hooks — pin slots from d3d9.h, none pinned yet)? Method: watchpoint the writer of the requested w/h at one chain creation call, then follow its readers.
- **Q3 — pixel-size constant census.** Which uploaded constants carry pixel/texel sizes (screen effects, bloom offsets) — computed for window size, wrong on native buffers. Uploads cross the slot-94 gate ([../view_rewrite.md](../view_rewrite.md)); shader addresses: [shader_ctab_map.md](shader_ctab_map.md).
- **Q4 — desc-readback fallout.** Any engine path comparing a surface desc against its config and rebuilding/resetting? `reset_hook` counters are the canary. The device itself is untouched by the plan, so divergence risk is engine-internal only.

## Facts already in hand

- Engine `Reset` is menu-res-change driven and carries the engine's own params ([d3d_device_slots.md](d3d_device_slots.md)) — untouched by this plan.
- Caller-side dirty caches live in the `Dx9_*` layer ([dx9_state_wrapper.md](dx9_state_wrapper.md)); creation-call substitution returns a different (bigger) pointer, which those caches store as-is — Q1's log plus live counters verify no size-keyed cache mis-hit.
- Carrier eye RT / snapshot RT / dumps are all created from `g_main_desc` (desc of the recorded main scene RT) — they follow a substituted main RT automatically (`src/carrier/eye_replay.cpp`).
- The main scene RT is recorded as "first slot-0 surface observed after boot/Reset, never the backbuffer" (pointer identity) — unaffected by size changes.
