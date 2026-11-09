# Carrier-owned render outputs

`src/carrier/vr/render_outputs.cpp`, always on. The engine's final frame of each pass lands in a **carrier-created front surface**, not the swapchain backbuffer. The swapchain becomes the carrier's monitor mirror. RE basis: [reverse_engineering/render_target_table.md](reverse_engineering/render_target_table.md).

## Takeover

- **Surfaces:** a front (render target, swapchain format) and a depth surface at the engine's render size ([render_resolution.md](render_resolution.md)), both with scene MSAA's sample count ([msaa.md](msaa.md)), plus a non-MSAA mirror and a resolve scratch surface.
- **Holders replaced:** table `+0x3ea4` / `+0x3e98` (front), `+0x3e94` (auto depth, plus `+0x3ea0` when it is the alias), and `g_Dx9AutoDepthRef`.
  - Each owning holder is transferred: Release the displaced surface and AddRef ours, mirroring the engine's own release accounting.
  - The Dx9 bind caches are left alone. They mirror device state, so our surfaces just miss once.
- **Takeover points** (main thread; takeover is idempotent):
  - the `SetupRenderTargets` exit (InlineHook wrap at entry; boot);
  - the `RenderSystem_CheckDevice` restore tail (MidHook `0x0074d087`), because Reset does not re-run setup;
  - a per-Present invariant check.
- **Reset:** the carrier drops its refs before Reset; released pointers are never Released again.
- **Check:** a takeover-time WARNING logs if the engine's main RT does not match the render size.
- **Result:** the gameplay composite and HUD (render-command op 6, pointers baked from `+0x3e98/+0x3e94`) and menus/loading (`LtiRenderer_EndSubmit`) all draw into the carrier front. The real backbuffer is never bound by the engine in steady state.

## Present params

Device policy is applied to a COPY of the engine's `D3DPRESENT_PARAMETERS` at CreateDevice and Reset (`vr/device_params.cpp`, called from `hooks/device.cpp`). The engine's stored copy stays as it asked, which keeps `RenderSystem_ApplySettings` consistent. The policy:
- vsync off → `D3DPRESENT_INTERVAL_IMMEDIATE`;
- the swapchain runs without MSAA (it only carries the mirror). The game's AA request is ignored: it only ever covered the composite + HUD, and with it on the depth-tested composited icons fail to draw (a game bug, also without the mod). The front's sample count is scene MSAA's — [msaa.md](msaa.md).

## Per-pass flow

- **1→2 (LEFT done):** capture the front into the shared ring ([shared_textures.md](shared_textures.md)); resolve the front into the mirror.
- **2→0 (RIGHT done):** capture the front.
- **Present:** blit the mirror into the swapchain, aspect-fit with black bars, before every Present. DISCARD requires writing the backbuffer each frame.
- **One Present per frame:** pass 1's present-prev is skipped via the engine's own `g_SuppressPresent` (`0x011755c3`, set before pass 1, cleared again before pass 2), so the remaining Present shows the current LEFT. Device-lost detection still rides on that Present.
- **Transition frame after every Reset:** its HDR exposure is invalid ([reverse_engineering/hdr_adaptation.md](reverse_engineering/hdr_adaptation.md)). It is not captured (the host re-presents the previous pair) and the monitor shows black. The capture runs before the transition flag clears at 2→0.

## Logs

`outputs: window active=… takeovers=… monitorBlits=… mirrorUpdates=… presentSkips=… transitionFrames=… failures=…` every 10 s; `outputs: took over engine outputs (<point>, …)` per takeover.
