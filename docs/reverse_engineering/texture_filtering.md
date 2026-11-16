# Texture filtering

Verdict (2026-11-16, static audit): **the game never uses anisotropic filtering.** No option, no code path — the third-party claim that AF "may not be working" is correct; it was never implemented.

- All sampler state goes through `Dx9_SetSamplerState` (23 call sites) or the device-init defaults in `Dx9State_ApplyDefaultRenderStates`; no D3DX effects are imported, so nothing sets sampler state behind the wrapper ([dx9_state_wrapper.md](dx9_state_wrapper.md)).
- World/material samplers: `RenderShell_ApplyViewSamplerStates` — MIN/MAG are only POINT or LINEAR, MIP LINEAR/POINT/NONE (decoding in its plate).
- `MAXANISOTROPY` is written once, at device init, to the D3D default `1` (`g_DefaultSamplerStates`; the `"MaxAnisotropy"` string is only its entry in `g_SamplerStateNames`).
- No other call site passes `D3DTEXF_ANISOTROPIC` (3) as MIN/MAGFILTER; the 3s in `RenderCmd_BindTextureAndSamplers` are `D3DTADDRESS_CLAMP`/`BORDER` values.
- Caveat: `RenderShell_PrecacheExecuteStream` (op 0x11) sets sampler states from the `precache\display%i.precache` command streams, only during precache. Stream contents not audited.
- The default/name tables (`g_DefaultSamplerStates`, `g_SamplerStateNames`, and the render-state/TSS siblings) double as enable masks: a state is seeded only if its name entry is non-null (plate on `Dx9State_ApplyDefaultRenderStates`).

## The texture record and the bind path

- Every bind is `Dx9_SetTexture(view->texture)` then `RenderShell_ApplyViewSamplerStates` (ESI = sampler, `TextureView *` on the stack, `ret 4`), from `RenderShell_BindViewTexture` (render-command ops 4 / 20, named textures) and `RenderShell_PickPass_Material`.
- `TextureView` (0x34 bytes, Ghidra `/RenderPath`, `game/texture.hpp`): the producers are on the struct's comment. What tells textures apart: `type` (`+0x2a`) is 1 = 2D asset, 2 = cube asset, 4 = render target / external (every RT producer writes it, but not the level counts), 0 = CPU-made single level. `levels` / `residentLevels` (`+0x0a` / `+0x0c`) are reliable for asset types only.
- Type-4 records' `levels`/`residentLevels`/`format` are stale. No type-4 writer sets them; each writes only `texture`, the 0xffff dims, `type` and OR'd clamp flags. The writers:
  - `FUN_0074a180` (renderer RTs: ColorBufferCopy etc.), `PgLtiBloom_CreateSurfaces`;
  - `FUN_0047d7e0` (clouds: `CloudTex`/`CloudSurf`), `FUN_00482660` (`WaterWakeProfile`), `FUN_00482b50`/`FUN_00482f10`/`FUN_00486180`/`FUN_0047dae0` (water: `HeightSurf`, `HeightSurfTwo`, `NormalSurf`, `FoamMaskSurf`);
  - `FUN_00751305` (a `CreateTexture` wrapper; its function boundary is misdetected in Ghidra).

  Live (2026-11-16): type-4 records reading "8 levels, DXT" were single-level `D3DUSAGE_RENDERTARGET` textures on the device (1280x720 `A16B16G16R16`, 16x16 `A16B16G16R16F`), and the vertex-sampler ones 128x128 `A32B32G32R32F` render targets. Trust `type`, not the level counts, to tell render targets apart.
- Who writes `samplerFlags`: for assets, the texture file (`INFO` chunk, copied unchanged by `FUN_00750c40`); for render targets, their producers OR in 6/7 at creation; for Scaleform UI (whose textures are ordinary asset entries, [gui_textures.md](gui_textures.md)), `FUN_004aeca0` rewrites them on every bitmap-fill draw from the fill's wrap and sample modes (clamp gives 6, point gives 1), discarding the file's bits. The game's own widgets (`Gui_DrawTexturedRect`, text glyph runs) haven't been checked.
- Flags `0x10/0x20/0x80` come unchanged from asset files; no reader found. OPEN: one of them may be a per-texture filtering request authored for the console pipeline that the PC renderer never reads. Test by correlating the bits with texture kinds in a live census. First data (2026-11-16, `debug_gui_census`): `0x10` (61 binds) and `0x20` (12) appear only on mipmapped DXT world textures (64x64 to 512x512), never on render targets or GUI. GUI loses them anyway, because its fill setup rewrites the flags.
- Vertex textures (op 4) bind at samplers 257–260 through the same path. `g_SamplerStateCache` and the shell's bound-view table (`+0x3fd4`) are sized for 261 samplers, so this is by design, not an overflow.

Workaround without code: force AF outside the game — DXVK `d3d9.samplerAnisotropy = 16` in `dxvk.conf` (Proton), or driver-level override on native D3D9. The mod's own: [../texture_filtering.md](../texture_filtering.md).
