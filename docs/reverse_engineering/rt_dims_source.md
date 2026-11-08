# Render surface census & dimension sources

Pre-work for [../render_resolution.md](../render_resolution.md): where the engine's sizes come from and who reads them. Static census 2026-10-11, reader classification 2026-11-07; live items remain OPEN below. Per-address facts (labels + plate comments) live in the Ghidra project. The output surfaces themselves: [render_target_table.md](render_target_table.md).

## Two size authorities (stock sets them equal)

- **Display = `g_RenderCfg_ScrW/H`** (0x00dfb328/0x00dfb32c, INI `[Render] ScrW/ScrH`, `RenderConfig_LoadIni` 0x00753480). Readers: window creation (`CreateWindowExA` at cfg size, code at 0x006313eb), `RenderSystem_ApplySettings` (cfg vs present params → display-mode lookup; mode not found → **writes cfg = 640×480**), `RenderSystem_CreateDevice` / `RenderSystem_CheckDevice` (both call `OnScreenResize(cfg)`), `SetupRenderTargets` (cfg → fields), and the mouse scale (`g_MouseScaleX/Y` = cfg ÷ client or desktop size, applied in WndProc `WM_MOUSEMOVE`). ApplySettings is the only code that compares dims against device state, and it uses cfg.
- **Render = RenderShell `+0x2b98/+0x2b9c`** (aliases `g_ScreenWidth/Height` 0x017d1688/0x017d168c). Writers: `PgLtiRendererPc_SetupRenderTargets` (0x007494e0, from cfg), `LtiRenderer_OnScreenResize` (0x007492f0, from its arguments = cfg), `RenderShell_StaticInit` (default). Everything render-side reads these, never cfg.
- So a render-only size change leaves the window, present params and ApplySettings stock; overriding cfg would move the window and break the mode lookup.
- Scale factor: `g_RenderScale` `+0x2bd4` (0x017d16c4, cvar hash 0x62c192d1, applied in `FUN_004c2bd0`) multiplies the fractional chain sizes; full-size chain surfaces use the raw fields.

## Render-field readers (classification)

Height readers mirror the width readers exactly. Field offsets scanned directly (the constant-use tool truncates).

- Chain sizing / viewports: SetupRenderTargets chain, bloom (`PgLtiBloom_CreateSurfaces`), clouds (`FUN_0047d7e0`), water (`FUN_00485c60`, code at 0x00485e70), half-res pass (`RenderShell_Begin/EndHalfResPass`, `RenderShell_DrawTexturedQuad`), main view viewport + H/480 scale (`FUN_0085a9b0`), `ViewContext_BuildCameraConstants` (viewport normalization).
- Screen-space 2D: UI ortho projection (`FUN_004aefd0`, pillarbox mode uses the aspect scales), Bink movie rects (`FUN_00709620`; `FUN_00709900` rescales them by new/old in OnScreenResize), UI shell half-dims (`*[0x00ed3574]+0x74/+0x78`).
- Camera aspect: `Fov_TanIndexHelper`, camera init `FUN_0069fdc0` (aspect cached into cam+0x4f0), projection builder `FUN_005efd30`, all via the aspect scales below.

## Aspect is snapped, not derived

OnScreenResize (and the mutated SetupRenderTargets stretch again) sets the 16:9 flag `+0x2bc4` and aspect scales `+0x2bc8/+0x2bcc` to the NEARER of 4:3 / 16:9 — never the true ratio. At 2016×2240 (0.9) the engine picks 4:3. Further writers: `FUN_005b0100` (script/cvar binding) and `FUN_005b2c70` (aspect-mode setter, modes 0–3). Any render-size change needs an explicit aspect policy.

## Q1 — surface census (static)

Backbuffer-derived:
- PgLtiRendererPc.cpp (`PgLtiRendererPc_SetupRenderTargets`): `_MainColorSurf` + `_DepthBufferCopySurface` (full), `_DownsampleColorSurfHDR` + `_DownsampleZBufSurf` (half), scene depth (D24S8, full, only in one depth mode — [render_target_table.md](render_target_table.md)). **CORRECTED live 2026-11-08: these are sized from the FRONT's `GetDesc`** (`0x0074a2b3`, the swapchain backbuffer at that point), **not from the fields.** The first native run (`render_size=2016x2240`) left them at 2560×1440 → black world. The render_size MidHook at `0x0074a2c1` rewrites that desc. Created in the plaintext tail after the mutated stretch.
- Field-derived (follow a field change automatically): the level-load sets below (bloom, clouds, water) — live at native: bloom 1008×1120…, water 672×746.
- PgLtiBloomPc.cpp (`PgLtiBloom_CreateSurfaces`): `_BlurH/VSurface[i]` = (W×k)/d, d∈{2,4,8,16}.
- PgCloudsWin32.cpp: `mCloudOutRT` (W×k/2). PgWaterWin32.cpp: `_pReflectionSurf`/`_pReflectionSurfZBuf` (W×k/3), `_pWaterDepthCopySurf` (full W/H).

Independent (fixed sizes — must NOT be resized; they ignore the fields):
- Shadow maps (`PgLtiShadow_CreateSurfaces`): 1024×4096. Water sim textures: 128×128. Clouds `mCloudSurfRT`: 512×512. Bloom downsample/luminance pyramid: 64/16/4 + 4×4 adaptive pair + `g_pAdaptiveDest` 4×4.
- `_pHeightSurf[i]` is created inside SecuROM VM code; its fixed-size siblings suggest it is fixed too.

NOT created from the fields: the front (`_FrontColorSurf`) and auto depth (`_BackDepthSurf`) are device `GetRenderTarget(0)` / `GetDepthStencilSurface` results, sized by the swapchain — [render_target_table.md](render_target_table.md).

`PrecacheSurface` (vtable 0x00bd6e68, factory `PrecacheSurface_CreateFromDesc` 0x007580d0, desc W@+0x10/H@+0x24/flags@+8): generic RT/DS/plain funnel; no census chain surface goes through it.

## Q3 — pixel-size constants (answered)

`RenderCmd_SetScreenConstants` (0x00852740, cmd op 8, record = viewport {X,Y,W,H,MinZ,MaxZ}) recomputes `g_ScreenConstsVS` (0x017d2aa0+: 2/W, −2/H) and `g_ScreenConstsPS` (0x017d2ab0+: W, 1/W, H, 1/H) and uploads them via SetVertex/PixelShaderConstantF, with register/count per shader taken from the shader records (+0xb4/+0xb8, +0xd4/+0xd8). These are the ctab `InvViewport`/`InvScrSize` registers ([shader_ctab_map.md](shader_ctab_map.md)). Producers: `ViewContext_BuildCameraConstants` (append 0x0085989b; W/H = ViewContext+0x18/+0x1c, normalized against `+0x2b98×k`) and the water-reflection variant (PgWater obj +0x14/+0x18 = W×k/3). Half-res never emits op 8: `RenderShell_BeginHalfResPass` hard-codes `g_ScreenWidth/2` and rewrites the same block. **All constants follow the fields.**

## Q4 — desc-readback fallout

The only known desc/GetViewport readback is the debug dump `RenderCmd_DumpTargets_Debug` (0x007533c0, flag-gated). No path compares a surface desc against the fields and rebuilds; ApplySettings compares cfg against present params only.

## OPEN (live instrumentation)

- Log CreateTexture/CreateRenderTarget/CreateDepthStencilSurface sizes+callers across gameplay AND menus to catch runtime-created surfaces the static census can't see (movie/HUD textures, menu rebuilds), and verify the classification live.
- Boot order — ANSWERED live 2026-11-08: `SetupRenderTargets` is entered first. Its leading `FUN_00749270` creates the device and calls `OnScreenResize(cfg)` (stack `0x0074d148<0x0074cdc1<0x0074c849<0x00749287<0x007494f5`). Then Setup copies cfg → fields and creates the chain. After boot: fields 2560×1440, flag169=1, aspect 1.7778, mouse scale 1.0. Reset/res-change order (live, run 3): Reset → `RenderSystem_CheckDevice` → `OnScreenResize(cfg)` (from `0x0074d07d`), with no `SetupRenderTargets`, so the stretch's aspect re-snap only happens at boot. 1920×1080 kept 16:9 (flag 1, aspect 1.7778). A non-16:9 render size remains untested.
- ~~OnScreenResize callee conventions~~ ANSWERED 2026-11-08 (Ghidra plates). OnScreenResize itself takes W on the stack (callee-popped), H in EAX and `this` in ESI. Its callees:
  - `RenderShell_SetupViewports` (`0x0085a9b0`): AX = view mode, ECX = `this`. The 16:9 flag matters only for the split-screen layout.
  - `FUN_00854da0` (×2): a **SecuROM VM thunk** with opaque register inputs.
  - `FUN_00609ef0`: 1 stack argument, callee-popped.
  - `FUN_00709900` (Bink rects): new/old ratios in XMM0/XMM1, computed before the field stores.

  Verdict: don't reimplement OnScreenResize. Rewrite its inputs and post-fix aspect/mouse ([../render_resolution.md](../render_resolution.md) §1).
- reset_hook counters as the canary for engine-internal rebuild loops once a size change is live.
