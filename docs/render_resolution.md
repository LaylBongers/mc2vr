# Render resolution (HMD-native per-eye rendering)

While the HMD is in use, the engine renders at the runtime's recommended per-eye size (2016×2240 on the current HMD). The window, present params and swapchain keep the game's own resolution. With the HMD on standby, or no host, the game renders exactly as unmodded. Code: `src/carrier/vr/render_size.cpp`, `src/carrier/vr/adaptation.cpp`; outputs in [render_outputs.md](render_outputs.md). RE basis: [reverse_engineering/rt_dims_source.md](reverse_engineering/rt_dims_source.md).

## Mechanism

The engine has separate display (`g_RenderCfg`) and render (`+0x2b98/+0x2b9c`) sizes; stock sets them equal. The carrier changes only the render size, by setting the inputs of the engine's own resize code. `OnScreenResize` calls a SecuROM VM thunk with opaque register inputs, so it is not reimplemented.

| Site | What the carrier does |
|---|---|
| `LtiRenderer_OnScreenResize` entry `0x007492f0` | W (`[esp+4]`) / H (EAX) → render size. The engine then writes the fields, float copies, viewports, UI half-dims and Bink rects consistently. |
| its epilogue `0x007494d5` | aspect `+0x2bc8/+0x2bcc` = true W/H (`render_aspect=snap` keeps the engine's snap); mouse scale × render/cfg |
| `SetupRenderTargets` field store `0x00749500` | ECX/EAX (cfg W/H) → render size |
| setup-chain desc `0x0074a2c1` | the front's GetDesc W/H → render size; it sizes main RT, depth copy, scene depth and downsample. Also covers device restore, which re-enters the same tail. |
| after setup (render_outputs' wrap) | re-apply aspect (the mutated stretch re-snaps it) |
| every Present | log render-field drift (a canary for unknown writers); re-apply aspect/mouse drift (script aspect setters) |

The carrier's own surfaces are created from the render fields. The view-rewrite pass gate keys on the live render size ([view_rewrite.md](view_rewrite.md)).

## Following the HMD

- **Size source.** `render_size=hmd` takes the size from the host's `MC2VR_MSG_VIEW_SIZE` ([ipc.md](ipc.md)), the OpenXR recommended image rect, and always follows the HMD. An explicit `render_size=WxH` follows by default (`render_size_follow_hmd`).
- **"In use"** means the session is VISIBLE/FOCUSED. That is the runtime's lifecycle signal, already debounced by the runtime (proximity sensor/standby), so the carrier adds no debounce. The per-frame tracked flag is deliberately excluded: a tracking drop only gives camera pass-through, never a device reset.
- **Switching** uses the engine's own device-lost recovery. On a state change the device hook answers `RenderSystem_CheckDevice`'s per-frame `TestCooperativeLevel` once with `D3DERR_DEVICENOTRESET` (only for calls from CheckDevice's range). The engine then releases, Resets, runs `OnScreenResize` (the entry hook applies the new target) and restores, and the carrier retakes its outputs. Each switch costs a short hitch.
- **Exposure continuity** (`adaptation.cpp`; RE: [reverse_engineering/hdr_adaptation.md](reverse_engineering/hdr_adaptation.md)):
  - the adaptation surfaces are saved (GetRenderTargetData → SYSTEMMEM) right before the forced reset;
  - they are written back (UpdateSurface) at the start of the first 3 frames after it, so the reset frame's bogus adaptation step does not persist;
  - the reset frame itself is hidden (the transition frame, [render_outputs.md](render_outputs.md)).

## Known limitations

- With `render_size_follow_hmd=off` the menu mouse is off at native size. The monitor shows the render image pillarboxed, while `g_MouseScaleX/Y` maps the full window. Fixing it means mapping through the fit rect (offset + uniform scale) in the `WM_MOUSEMOVE` path (`g_WindowOriginX/Y`, near `0x006310e6`). In the default follow mode the mouse is stock whenever it matters.
- About +22% pixels per pass at 2016×2240 vs 2560×1440. Gameplay frame time was ~14–15 ms vs ~11–13 ms.
- The HUD is laid out by the engine at the per-eye aspect (0.9); it renders correctly ([hud.md](hud.md) for the remaining stereo HUD issues).
- Scene MSAA: [msaa.md](msaa.md).

## Rejected approaches

- **Device creation-call substitution:** viewports and screen constants stay config-sized.
- **Overriding `g_RenderCfg`:** the window is created at cfg size, and the mode lookup fails and falls back to 640×480.
- **A native-size swapchain:** fullscreen present params; 2016×2240 is no display mode.
- **The render fields alone:** the setup chain is sized from the front's desc, and the backbuffer-bound draws and auto depth stay swapchain-sized.
- **A device-level virtual swapchain** (getter substitution): it fakes device state. Superseded by owning the engine's target table.
