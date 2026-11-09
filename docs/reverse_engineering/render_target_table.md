# Render target table (RenderShell outputs)

The engine's frame outputs live in a pointer table on `g_RenderShell` (`0x017ceaf0`), `+0x3e90..+0x3eb0`. Per-slot facts are in the plate at `g_RT_MainColorSurf` (`0x017d2980`). How the mod takes it over: [../render_outputs.md](../render_outputs.md). Sizes: [rt_dims_source.md](rt_dims_source.md).

## Shape

| Slot | Contents |
|---|---|
| `+0x3e90` | main color = main texture level 0 (fp16, ms0) |
| `+0x3e94` | auto depth = device `GetDepthStencilSurface` (swapchain-sized; owns a ref) |
| `+0x3e98` | front ref = copy of `+0x3ea4` (released separately) |
| `+0x3e9c` | depth copy (full size) |
| `+0x3ea0` | scene depth: own D24S8 when `g_LtiRenderer+0x594 != 0` (the MSAA level; 8 with MSAA on), else an alias of `+0x3e94` (not released separately) |
| `+0x3ea4` | front = device `GetRenderTarget(0)` = swapchain backbuffer |
| `+0x3ea8/+0x3eac/+0x3eb0` | half-size downsample depth/color (+ texture) |

Other holders of the swapchain surfaces: `Dx9State_SeedCachesFromDevice` (`0x00752f70`, at device creation and restore) keeps an auto-depth ref in `g_Dx9AutoDepthRef` (`0x01175318`). It also seeds the bind caches `g_DepthStencilCache` / `g_CurRenderTarget`; those only skip identical binds.

## Lifecycle

- Create: `PgLtiRendererPc_SetupRenderTargets` (vtbl slot 6). It is thiscall with 1 stack arg, callee-cleaned; caller `0x004c0050`. Its exit runs through a SecuROM-mutated stretch, so wrap it at entry. The fetches and the chain creation are in the plaintext tail (`0x0074a1cc..0x0074a84d`); the chain is sized from the front's desc.
- Release: `LtiRenderer_ReleaseRenderTargets` (`0x0074a850`, alias-aware), from `LtiRenderer_ReleaseDevice` (vtbl slot 7) and from `RenderSystem_ReleaseDeviceResources` (`0x0074d1b0`, pre-Reset). The latter also releases `g_Dx9AutoDepthRef` and the registries, and binds the real backbuffer directly on the device. `LtiRenderer_Shutdown` drops the front ref.
- Restore: `RenderSystem_RestoreDeviceResources` (`0x0074d410`) re-enters the setup tail and refills the table; order in [rt_dims_source.md](rt_dims_source.md).

## Consumers (read at use time)

A full plaintext scan of the slot offsets ([ghidra-reva.md](ghidra-reva.md)) finds only these; no other copies exist.

- `LtiRenderer_BeginSubmit`: RT0 = `+0x3e90`, depth = `+0x3ea0`.
- `LtiRenderer_EndSubmit`: StretchRect main → `+0x3ea4`, then SetRenderTarget(front) — the menu/loading path.
- **Render-command op 6** (SetRenderTargets, `0x00856a95`: color `[rec+4]`, depth `[rec+8]`) — the gameplay composite and HUD. `PgLtiBloom_EmitPasses` bakes `+0x3e98` / `+0x3e94` into these records producer-side, before the stereo passes. Both passes replay the same pointer, so a per-pass table swap cannot redirect it.
- Render-command target switch case 7 (`0x008563e4`, `0x00856847`): `+0x3e90`.
- Clouds code (`PgClouds_EmitRecords` / `PgClouds_EmitViewRecords`): `+0x3e90` / `+0x3ea0`.
- Device getters: only the setup tail (`0x0074a1d8` / `0x0074a24c`) and the cache seeding; no runtime getter path.

Every creation site also registers its surface by name with the PrecacheSurface registries (`g_SurfaceRegistryPtrA/B`; `vfn[+0x1c]` = register(hash, surf, 3), which re-records when the pointer differs). With the front replaced, no binds through these entries were observed.

## OPEN

- Which code, if any, looks registry entries up by name.
