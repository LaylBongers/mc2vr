# Render target table (RenderShell outputs)

The engine's frame outputs live in a small pointer table on `g_RenderShell` (`0x017ceaf0`), `+0x3e90..+0x3eb0`. Static RE 2026-11-07; per-slot facts are the plate at `g_RT_MainColorSurf` (`0x017d2980`) in Ghidra. The mod plan built on it is in [../render_resolution.md](../render_resolution.md); the render-size inputs are in [rt_dims_source.md](rt_dims_source.md).

## Shape

- Slots: main color (`+0x3e90`, fp16 main RT), auto depth (`+0x3e94`, device `GetDepthStencilSurface`), front ref (`+0x3e98`), scene depth (`+0x3ea0`), front (`+0x3ea4`, device `GetRenderTarget(0)` = swapchain backbuffer), and four further chain surfaces (`+0x3e9c/+0x3ea8/+0x3eac/+0x3eb0`).
- **Two references to the backbuffer**: `+0x3ea4` and `+0x3e98` (a copy made at `0x0074a3f5`, released separately by `LtiRenderer_Shutdown`). Anything that replaces the front must replace both.
- **Two depth modes**: `g_LtiRenderer+0x594 != 0` → `+0x3ea0` is its own D24S8 surface at render size; otherwise it aliases the auto depth `+0x3e94` (swapchain-sized). `LtiRenderer_ReleaseRenderTargets` skips the release when they are equal.

## Lifecycle

- Create: `PgLtiRendererPc_SetupRenderTargets` (vtbl slot 6). The chain itself is created inside the SecuROM-mutated stretch; the front/depth/main fetches are in the plaintext tail after it (`0x0074a1cc..0x0074a45c`).
- Release: `LtiRenderer_ReleaseRenderTargets` (`0x0074a850`) — from `LtiRenderer_ReleaseDevice` (vtbl slot 7) and from the device-lost path (`RenderSystem_CheckDevice` → `FUN_0074d1b0`). `LtiRenderer_Shutdown` additionally drops the front ref.
- Restore order on device restore: release → reset/recreate → `LtiRenderer_OnScreenResize(g_RenderCfg)` → setup.

## Consumers (read at use time)

A full plaintext instruction scan for the slot offsets (method: [ghidra-reva.md](ghidra-reva.md)) found only:

- `LtiRenderer_BeginSubmit`: RT0 = `+0x3e90`, depth = `+0x3ea0`, every frame.
- `LtiRenderer_EndSubmit`: `+0x3ea4` (RT0 → front copy when they differ).
- Render-command target switch (case 7 at `0x008563e4`, `0x00856847`): `+0x3e90`.
- `FUN_0047db80` / `FUN_0047ded0` (clouds area, next to `PgCloudsWin32` `FUN_0047d7e0`): `+0x3e90` / `+0x3ea0`.
- `FUN_0074fae0` (4 KB, indirect-only): `+0x3e94` and `+0x3e98` — bakes them into render-command op-6 records (live verdict below).

No other plaintext copies of the slots exist. Every creation site also registers its surface by name with a registry object (`g_SurfaceRegistryPtrA/B`), a name-keyed cache (live verdict below).

## Live verdicts (rt_probe run 2026-11-08, 2560×1440)

- **Composite bind = baked render command.** The gameplay front bind is `RenderCmd_ExecuteStream` op 6 (`0x00856a95`: color = `[rec+4]`, depth = `[rec+8]`). `FUN_0074fae0` fills those pointers producer-side, before the passes, from `+0x3e98` / `+0x3e94`: its reads match op-6 front binds 1:1 per pass in every steady window. Both passes replay the same baked pointer, so **swapping the table between passes cannot redirect it**.
- The other front path is `LtiRenderer_EndSubmit` (StretchRect main → front at `0x0074acad`, then SetRenderTarget(front) at `0x0074acba`). It reads `+0x3ea4` live and runs in menus and loading screens.
- No device-getter path: `GetRenderTarget` / `GetDepthStencilSurface` are called once each at the setup tail (`0x0074a1d8` / `0x0074a24c`); `GetBackBuffer` / `GetSwapChain` are called only by the carrier.
- Run 2 (device hooked at creation) also caught the getters that run before the table exists: `Dx9State_SeedCachesFromDevice` (`0x00752f70`, called from `RenderSystem_CreateDevice`) seeds `g_DepthStencilCache` with the auto depth (keeping a ref in `g_Dx9AutoDepthRef` `0x01175318`) and `g_CurRenderTarget` with the backbuffer. That makes a third holder of the auto-depth pointer, so replacing it must cover this cache-seed too (or happen before it). The caches only short-circuit identical binds, so a replacement pointer just misses once.
- **Depth mode:** `g_LtiRenderer+0x594 = 8` with an 8×-MSAA swapchain. The backbuffer and auto depth have ms=8; the main chain has ms=0. Scene depth is its own D24S8 surface (not aliased), so the auto depth is only paired with the front, in op 6.
- **Registries** are name-keyed PrecacheSurface caches: B's `vfn[+0x1c]` = `PrecacheSurface.cpp` register(hash, surf, 3), which re-records an entry when the pointer differs. That makes them a lookup path; re-registering a replacement under the same name updates them.
- Boot order: the probe saw `SetupRenderTargets` entered first. Its leading call `FUN_00749270` creates the device (→ `RenderSystem_ApplySettings` → `RenderSystem_CreateDevice` → `OnScreenResize(cfg)`). Only then does Setup copy cfg → fields and create the chain; [rt_dims_source.md](rt_dims_source.md).
- Creation census matches the static census: 35 unique DEFAULT-pool creations, no runtime surprises. Level loads re-create the bloom/water/clouds sets (25 creations) at the same sizes.

- **Reset path** (run 3, menu resolution change 2560×1440 → 1920×1080 → back):
  1. `RenderSystem_ApplySettings` builds new params and the device is Reset.
  2. `RenderSystem_CheckDevice` restores: `OnScreenResize(cfg)`, then `RenderSystem_RestoreDeviceResources` (`0x0074d410`, mutated), then `FUN_00753050`.

  The restore re-seeds the Dx9 caches (`Dx9State_SeedCachesFromDevice` again) and refills the table with the new swapchain surfaces, re-creating 47 DEFAULT surfaces. **`SetupRenderTargets` is not re-run on Reset**, so a hook on it alone misses resets.
- Registry: with the carrier owning the front (run 3), zero binds of the real backbuffer occurred outside the one-frame reset transition. Nothing looks the registry entries up for binding.

## OPEN

- Which code (if any) looks up registry entries by name — no consumer has been seen in rendering.
