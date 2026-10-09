# Dx9 state wrapper

Every D3D call in the render path goes through `g_LtiRenderer->dx9State` (`+0x5bc`; global `0x01175288` typed `LtiRenderer *`). Slot map = `Dx9StateWrapper_vtbl` struct members + the `g_LtiRenderer` plate. Vtable recipe: [vtables.md](vtables.md).

- The vtable is plain IDirect3DDevice9 order — wrapper slot n == device slot n for every slot (an earlier "omits one method / n+1" claim was wrong), and `dx9State` is the raw device object (the carrier's device VmtHook sees these calls; slots: [d3d_device_slots.md](d3d_device_slots.md)).
- Dirty-tracking caches are caller-side in the `Dx9_*` functions, updated after each device call — observing/forwarding at the device vtable is safe, ALTERING values there would desync `g_RenderStateCache` (0x105 entries, cleared by the invalidate slot) and the texture/sampler/RT caches. Alter at the `Dx9_*` function level instead.
- Precache: plates on `RenderShell_PrecacheLoadStep`/`RenderShell_PrecacheFinish`; `g_SuppressPresent` suppresses Present during precache frames.
- `LtiRenderer_EndSubmit` StretchRects RT0 → backbuffer (`LtiRenderer+0x3ea4`) whenever they differ — existing RT→backbuffer seam for the eye-image capture (also noted in [../stereo.md](../stereo.md)).

## Open

- Wrapper vtable ADDRESS (type annotation DONE): the vtable is the D3D9 device's own (runtime-installed by `Direct3DCreate9`/`CreateDevice`, so no static address); remaining `slotNN` members can simply be named from d3d9.h order.
