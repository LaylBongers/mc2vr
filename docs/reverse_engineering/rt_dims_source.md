# Render surface census & dimension sources

Pre-work for [../render_resolution.md](../render_resolution.md): which creation calls the size-substitution rewrites, and whether engine CPU-side sizes self-adapt. Static census done 2026-10-11 (this note); live instrumentation items remain OPEN below. Per-address facts (labels + plate comments) live in the Ghidra project.

## Verdict: dims are 100% config-driven (Q2 answered)

No desc/GetViewport readback drives any dims. Everything flows from ONE pair of RenderShell/LtiRenderer fields, `+0x2b98`/`+0x2b9c` (aliases of globals `g_ScreenWidth` 0x017d1688 / `g_ScreenHeight` 0x017d168c):

- Root: INI `[Render] ScrW/ScrH` → `g_RenderCfg_ScrW/H` (0x00dfb328/0x00dfb32c, `RenderConfig_LoadIni` 0x00753480 from WinMain). **Present params are also built from this pair — substituting here would resize the swapchain.**
- Copy into the fields: `PgLtiRendererPc_SetupRenderTargets` (0x007494e0) and **`LtiRenderer_OnScreenResize` (0x007492f0) — the res-change seam.** OnScreenResize additionally updates float W/H copies `+0x3f20/+0x3f50`/`+0x3ef4/+0x3f54`, 16:9 flag `+0x2bc4`, aspect scales `+0x2bc8/+0x2bcc`, half-dims into `*[0x00ed3574]+0x74/+0x78`, then FUN_0085a9b0 / FUN_00854da0×2 / FUN_00709900.
- Scale factor: `g_RenderScale` `+0x2bd4` (0x017d16c4, cvar hash 0x62c192d1, applied in `FUN_004c2bd0`) — multiplies every fractional chain size; full-size chain surfaces use the raw fields.

So substitution at the creation hooks alone would NOT propagate (engine would set a 2560×1440 viewport + screen constants on native surfaces). Two candidate seams, decided in [../render_resolution.md](../render_resolution.md): substitute at creation-call args (needs the viewport/constants rescale hooks anyway), or substitute the `+0x2b98/+0x2b9c` store inside OnScreenResize/SetupRenderTargets (creation hooks then become unnecessary; viewports, half-res, screen constants, bloom/clouds/water fractions and camera-constant normalization all follow the fields automatically).

## Q1 — surface census (static)

Backbuffer-derived (request sizes come from the fields → follow a field substitution automatically):
- PgLtiRendererPc.cpp (`PgLtiRendererPc_SetupRenderTargets`): `_MainColorSurf` + `_DepthBufferCopySurface` (full), `_DownsampleColorSurfHDR` + `_DownsampleZBufSurf` (half). Created through a SecuROM-mutated stretch (0x007495b3–0x0074a17f) — hook the plaintext creation calls, not the stretch.
- PgLtiBloomPc.cpp (`PgLtiBloom_CreateSurfaces`): `_BlurH/VSurface[i]` = (W×k)/d, d∈{2,4,8,16}.
- PgCloudsWin32.cpp: `mCloudOutRT` (W×k/2). PgWaterWin32.cpp: `_pReflectionSurf`/`_pReflectionSurfZBuf` (W×k/3), `_pWaterDepthCopySurf` (full W/H).

Independent (fixed sizes — must NOT be resized; they self-exclude from any field-based seam):
- Shadow maps (`PgLtiShadow_CreateSurfaces`): 1024×4096. Water sim textures: 128×128. Clouds `mCloudSurfRT`: 512×512. Bloom downsample/luminance pyramid: 64/16/4 + 4×4 adaptive pair + `g_pAdaptiveDest` 4×4.
- `_pHeightSurf[i]` is created inside SecuROM VM code — unhookable at creation; fixed-size siblings make size-keying a safe exclusion.

NOT creation calls: `_FrontColorSurf`/`_BackDepthSurf` = wrapper `GetRenderTarget` (+0x98) / `GetDepthStencilSurface` (+0xa0) — device-state aliases; can't be size-substituted, only redirectable at bind (SetRenderTarget) — matches the composite-redirect part of the plan.

`PrecacheSurface` (vtable 0x00bd6e68, factory `PrecacheSurface_CreateFromDesc` 0x007580d0, desc W@+0x10/H@+0x24/flags@+8): generic RT/DS/plain funnel, but no census chain surface goes through it — no hook needed there.

## Q3 — pixel-size constants (answered)

`RenderCmd_SetScreenConstants` (0x00852740, cmd op 8, record = viewport {X,Y,W,H,MinZ,MaxZ}): recomputes `g_ScreenConstsVS` (0x017d2aa0+: 2/W, −2/H) and `g_ScreenConstsPS` (0x017d2ab0+: W, 1/W, H, 1/H) and uploads via SetVertex/ PixelShaderConstantF with per-shader register/count from shader records (+0xb4/+0xb8, +0xd4/+0xd8) — the ctab [shader_ctab_map.md](shader_ctab_map.md) `InvViewport`/`InvScrSize` registers. Producers: `ViewContext_BuildCameraConstants` (append 0x0085989b; W/H = ViewContext+0x18/+0x1c, values set by VM-side push code but normalized against `+0x2b98×k` in the same function → follow the fields) and the water-reflection variant (PgWater obj +0x14/+0x18 = W×k/3). Half-res never emits op 8 — `RenderShell_BeginHalfResPass` hard-codes `g_ScreenWidth/2` and rewrites the same constant block. **All constants follow the fields; no upload-gate rewrite needed if the field seam is used.**

## Q4 — desc-readback fallout

Only known desc/GetViewport readback is the debug dump `RenderCmd_DumpTargets_Debug` (0x007533c0, prints "Viewport = ..." / "RenderTarget[i] = ...", gated by a debug flag). No engine path compares a surface desc against config and rebuilds — statically. Live verification still pending (reset_hook counters as canary).

## OPEN (live instrumentation)

- Log CreateTexture/CreateRenderTarget/CreateDepthStencilSurface sizes+callers across gameplay AND menus to catch runtime-created surfaces the static census can't see (movie/HUD textures, menu rebuilds) — and verify the census classification live.
- reset_hook counters as the Q4 canary once a substitution is live.
- Menu res-change: confirm OnScreenResize runs per rebuild and carries the config pair (callers FUN_0074ce9f/FUN_0074d0d0 read `g_RenderCfg`), i.e. the field seam survives res changes without extra hooks.
