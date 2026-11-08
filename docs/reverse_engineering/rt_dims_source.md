# Render size sources

Where the engine's sizes come from and who reads them. The output surfaces themselves: [render_target_table.md](render_target_table.md). The mod built on this: [../render_resolution.md](../render_resolution.md). Per-address facts are in the Ghidra plates.

## Two size authorities (stock sets them equal)

- **Display = `g_RenderCfg_ScrW/H`** (`0x00dfb328`/`0x00dfb32c`, INI `[Render] ScrW/ScrH`, `RenderConfig_LoadIni` `0x00753480`). Readers:
  - window creation (`CreateWindowExA` at cfg size, code at `0x006313eb`);
  - `RenderSystem_ApplySettings`: cfg vs the stored present params → display-mode lookup; mode not found → it **writes cfg = 640×480**;
  - `RenderSystem_CreateDevice` / `RenderSystem_CheckDevice`: both call `OnScreenResize(cfg)`;
  - `SetupRenderTargets` (cfg → fields);
  - the mouse scale `g_MouseScaleX/Y` = cfg ÷ client (windowed) or desktop size, applied in WndProc `WM_MOUSEMOVE`.
- **Render = RenderShell `+0x2b98/+0x2b9c`** (= `g_ScreenWidth/Height` `0x017d1688`/`0x017d168c`). Writers: `PgLtiRendererPc_SetupRenderTargets` (from cfg), `LtiRenderer_OnScreenResize` (from its arguments = cfg), `RenderShell_StaticInit` (default). Render-side code reads these, never cfg.
- `RenderSystem_ApplySettings` compares the LtiRenderer's stored present-param copy field by field (size, `MultiSampleType` @+0x10, refresh, interval @+0x34) against its settings. An in-place patch of the params handed to CreateDevice/Reset therefore reads as "settings changed".
- `g_RenderScale` (`+0x2bd4`, `0x017d16c4`) multiplies the fractional chain sizes; full-size surfaces use the raw fields.

## Who sizes what

- **Setup chain: sized from the FRONT's `GetDesc`, not from the fields.** The plaintext tail of `SetupRenderTargets` calls GetDesc on `+0x3ea4` (the swapchain backbuffer at that point, `0x0074a2b3`). That desc sizes the main texture, depth copy, scene depth and the half-size downsample targets. Device restore re-enters the same tail, so this holds after every Reset too.
- **Field-derived** (follow a field change automatically): bloom `_BlurH/VSurface[i]` = (W×k)/d, d∈{2,4,8,16}; clouds `mCloudOutRT` (W×k/2); water `_pReflectionSurf`/`ZBuf` (W×k/3) and `_pWaterDepthCopySurf` (full); the half-res pass; main view viewport (`RenderShell_SetupViewports`); screen constants (below). These are created at level load and on device restore.
- **Fixed** (must not be resized): shadow maps 1024×4096; water sim 128×128; clouds `mCloudSurfRT` 512×512; bloom luminance pyramid 64/16/4 and the 1×1 adaptation surfaces ([hdr_adaptation.md](hdr_adaptation.md)). `_pHeightSurf[i]` is created in VM code; fixed by its siblings.
- **Swapchain-sized**: the front (`GetRenderTarget(0)`) and auto depth (`GetDepthStencilSurface`).

## Render-field readers

Height readers mirror the width readers.

- Chain sizing / viewports: the field-derived sets above, `RenderShell_Begin/EndHalfResPass`, `RenderShell_DrawTexturedQuad`, `RenderShell_SetupViewports` (also H/480 UI scale), `ViewContext_BuildCameraConstants` (viewport normalization).
- Screen-space 2D: UI ortho projection (`FUN_004aefd0`, pillarbox mode uses the aspect scales), Bink movie rects (`FUN_00709620`; `FUN_00709900` rescales them by new/old in OnScreenResize), UI shell half-dims (`*[0x00ed3574]+0x74/+0x78`).
- Camera aspect: `Fov_TanIndexHelper`, camera init `FUN_0069fdc0` (cached into cam+0x4f0), projection builder `FUN_005efd30` — all via the aspect scales.

## Aspect is snapped, not derived

`OnScreenResize` (and the mutated SetupRenderTargets stretch, at boot) sets the 16:9 flag `+0x2bc4` and the aspect scales `+0x2bc8/+0x2bcc` to the NEARER of 4:3 / 16:9, never the true ratio (0.9 → 4:3). Other writers: `FUN_005b0100` (script/cvar binding), `FUN_005b2c70` (aspect-mode setter, modes 0–3). The flag only selects the split-screen layout in `RenderShell_SetupViewports`; single view ignores it.

## `LtiRenderer_OnScreenResize` (`0x007492f0`)

- Convention: W on the stack (callee-popped, `ret 4`), H in EAX, `this` (= g_RenderShell) in ESI.
- It computes the new/old ratios before storing the fields; they go to `FUN_00709900` in XMM0/XMM1.
- Callees:
  - `RenderShell_SetupViewports` (AX = view mode 1/2, ECX = this);
  - `FUN_00854da0` ×2: a **SecuROM VM thunk** with opaque register inputs, which is why the function is not reimplemented;
  - `FUN_00609ef0` (1 stack arg, callee-popped);
  - `FUN_00709900`.
- Epilogue at `0x007494d5` (single branch target).

## Boot and reset order

- Boot: `SetupRenderTargets` is entered first. Its leading `FUN_00749270` creates the device (→ `RenderSystem_ApplySettings` → `RenderSystem_CreateDevice` → `OnScreenResize(cfg)` → `Dx9State_SeedCachesFromDevice`). Setup then copies cfg → fields and builds the chain.
- Reset (device lost, menu resolution change, or carrier-forced):
  1. `RenderSystem_CheckDevice` → `RenderSystem_ReleaseDeviceResources` → Reset;
  2. `OnScreenResize(cfg)`;
  3. `RenderSystem_RestoreDeviceResources` (mutated) — re-seeds the Dx9 caches, re-enters the setup tail, re-creates about 47 DEFAULT surfaces;
  4. `FUN_00753050` (default render states).

  `SetupRenderTargets` itself is not re-entered.
- `RenderSystem_CheckDevice` runs every frame (render task, `0x0046a290`) and calls `TestCooperativeLevel`. `D3DERR_DEVICENOTRESET` takes the restore path.

## Pixel-size constants

`RenderCmd_SetScreenConstants` (`0x00852740`, cmd op 8, record = viewport) recomputes `g_ScreenConstsVS` (2/W, −2/H) and `g_ScreenConstsPS` (W, 1/W, H, 1/H). Register/count per shader come from the shader records — the ctab `InvViewport`/`InvScrSize` registers ([shader_ctab_map.md](shader_ctab_map.md)).
- Producers: `ViewContext_BuildCameraConstants` (normalized against `+0x2b98×k`) and the water reflection (W×k/3).
- `RenderShell_BeginHalfResPass` writes the same block with `g_ScreenWidth/2`.
- All constants follow the fields.

## Desc readbacks

Besides the setup chain's front GetDesc above, the only one is the flag-gated debug dump `RenderCmd_DumpTargets_Debug` (`0x007533c0`).
