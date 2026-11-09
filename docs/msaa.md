# Scene MSAA

Scene MSAA: done, `msaa=4` by default (`vr/msaa.cpp`). One small item is still OPEN, at the end.

## Problem

The PC port's MSAA option has no effect on the 3D scene. This is a known port bug; forcing MSAA in the driver reportedly works. The engine's HDR scene renders into the fp16 main texture (`g_RT_MainColorSurf`, `+0x3e90`), and D3D9 textures cannot be multisampled. The MSAA setting (`g_LtiRenderer+0x594`, the sample count) only reaches:
- the swapchain backbuffer and its auto depth, and therefore only the composite and HUD drawn there (render-command op 6). With it on, depth-tested composited icons (world markers) fail to draw — reproducible without the mod. The carrier ignores the request ([render_outputs.md](render_outputs.md)): at HMD size the 8× composite + resolve cost ~0.5 ms/frame for HUD edges only;
- the choice of a separate non-MSAA scene depth (`+0x3ea0`).

RE: [reverse_engineering/render_target_table.md](reverse_engineering/render_target_table.md).

## How the scene's targets are bound (RE, static)

Per-address facts are on the Ghidra plates and pre-comments (`RenderShell_SetViewType` tables `0x856984` / `0x8569a4`, `0x0074a4fb`, `0x0074fc15`, `0x0074a404`).

- **Single render target:** the scene draws to main color `+0x3e90` with scene depth `+0x3ea0`, one target (no MRT). The only multi-target pass is an offscreen pass without depth (`FUN_00483070`, view type 7, inferred).
- **Pointers baked into records:** besides the draw-time bind in `LtiRenderer_BeginSubmit`, six emitters copy the `+0x3e90` / `+0x3ea0` pointers into op-6 / op-7 records at build time. That includes the post chain drawing back into main (`0x74f85b`). Swapping the table therefore redirects post-processing too; the substitution has to happen when a bind executes, not in the table.
- **Original code binds main too, at draw time:** the view-type-7 exit in `SetViewType`, `RenderCmd_EndSurfacePass` and `RenderShell_EndHalfResPass`. The last two pair main with the **auto depth** `+0x3e94`, not the scene depth.
- **Depth copy is a separate pass:** view type 1 binds "DepthBufferCopy" (`+0x3e9c`, full-size G32R32F) as the only target, keeps the scene depth bound with Z writes on, and writes linear depth: effectively a Z-prepass. Under MSAA that pairs a 1× target with an MSAA depth, which D3D9 doesn't allow. Its order relative to the scene draws (types 5/6) is not checked yet.
- **Depth is never sampled:** no INTZ/RAWZ formats. Post effects read depth only from DepthBufferCopy.
- **Reads of the finished scene:** the post chain starts with op 12 (`ViewType9_StretchBlit`), a StretchRect of the bound RT0 into "ColorBufferCopy" (`+0x3fa4`). Refraction materials (view type 9) do the same mid-scene. A StretchRect from a multisampled surface is itself a resolve. The main texture "ColorBuffer" (`+0x43f0`) is also reachable by name for sampling; no static binder was found, so that needs a runtime check.
- **The engine's MSAA setting:** it only gives `+0x3ea0` its own depth surface, and that surface isn't multisampled either. The engine has no resolve code.

## What a stereo pass binds (runtime; a one-frame bind census, removed since, in git history)

With the in-game AA option off, the scene depth `+0x3ea0` is the auto-depth alias, i.e. the carrier depth. Per pass, in order:
- small offscreen passes, then the **depth prepass**: RT0 = DepthBufferCopy with the scene depth, view types 1–3, depth writes on, `LESSEQUAL`;
- the shadow atlas (own 1024×4096 depth);
- the **scene**: RT0 = main with the scene depth, view types 5/6, `LESSEQUAL`, mostly without depth writes. It relies on the prepass depth;
- StretchRect main → ColorBufferCopy (full size), StretchRect DepthBufferCopy → a full-size G32R32F copy, one more draw into DepthBufferCopy, alpha draws into main (type 8), more full-size main → ColorBufferCopy copies;
- the post chain into its own 1× targets without depth, one draw back into main;
- the **composite** into the front with the scene depth (58 draws, some depth-tested: the world markers).

The main texture is never bound to a sampler; DepthBufferCopy's texture is. Wherever a 1× target is bound with the scene depth there are no draws.

## Design

`src/carrier/vr/msaa.cpp`, conf `msaa=off|2|4|8`, default 4.
- **Carrier front and depth** ([render_outputs.md](render_outputs.md)) are created multisampled. The depth is the scene depth, so the prepass, the scene and the composite all test the same MSAA depth. An own engine scene depth (in-game AA on) is mapped onto it, since nothing samples it.
- **Stand-ins** for main and DepthBufferCopy, which are texture levels and can't be multisampled. When the engine binds one as RT0, the device gets the stand-in. It is resolved (StretchRect) into the real surface before a read: as a StretchRect source, when its texture is bound to a sampler, and when it is unbound while its texture is still on a sampler.
- **Multisample rasterization:** the engine sets `D3DRS_MULTISAMPLEANTIALIAS` to its own AA setting (FALSE with the in-game option off) at device setup and restore, which makes every sample of a pixel identical. Our `BeginSubmit` sets it TRUE each pass through `Dx9_SetRenderState`, so the engine's render-state cache agrees.
- **Depth follows RT0:** the MSAA depth is bound only with an MSAA RT0; a 1× RT0 gets no depth.
- **Where:** the core device hooks (`SetRenderTarget`, `SetDepthStencilSurface`, `StretchRect`, `SetTexture`, [hooks.md](hooks.md)). The engine only ever sees its own surfaces, and its bind caches stay valid because the mapping is the same for every bind.
- The supported sample count is checked for all four formats at the first takeover; unsupported = off, logged.
- Log: `msaa: Nx resolves=… staleBinds=… failures=…` every 10 s. `staleBinds` counts binds after the real surface was written directly (a StretchRect destination), which the stand-in doesn't see; not expected.

Live (2026-11-09, full HMD size 2016×2240, Index at 90 Hz):
- **Quality:** a clear improvement already at 2×. Menus render correctly. World markers depth-test correctly; they are broken with the in-game AA option, which never multisampled the scene depth.
- **2×:** holds 90 Hz with no late frames. Main-scene GPU time per pass is within noise of the non-MSAA baseline (3.5–3.6 / 2.3 ms against 3.6 / 2.5).
- **8×:** about +1 ms of GPU time per pass over 2× (4.4–4.6 / 3.3–3.5 ms), 72–87 Hz: below the display rate, so SteamVR halves the rate ([pacing.md](pacing.md)).
- **4× (default):** 87–90 Hz with no late frames (first run, with `debug_gui_census` on): at the edge of the 90 Hz budget. Chosen ahead of the HUD/GUI pipeline work, which is expected to free GPU time.

OPEN:
- The DepthBufferCopy resolve averages linear depth at edges; post effects reading it may show thin halos. None reported so far.
