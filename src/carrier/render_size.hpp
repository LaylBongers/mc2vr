#pragma once

#include <cstdint>

// Render-size policy (docs/render_resolution.md §1, track 4).
//
// The engine keeps two size authorities: display = g_RenderCfg (window,
// present params, mode lookup) and render = RenderShell +0x2b98/+0x2b9c
// (every render-chain surface, viewport and screen constant). Stock sets them
// equal. With render_size=WxH the carrier makes the render size W×H while the
// display size stays stock, by setting the INPUTS of the engine's own resize
// code (not reimplementing it — OnScreenResize calls a SecuROM VM thunk with
// opaque register inputs):
//   - LtiRenderer_OnScreenResize entry (W on the stack, H in EAX) -> W×H;
//   - its epilogue -> aspect policy (+0x2bc8/+0x2bcc = true W/H instead of the
//     4:3/16:9 snap) and mouse scale × render/cfg (menu mouse in render space);
//   - SetupRenderTargets' main-chain desc (the engine sizes main RT, depth
//     copy, scene depth and downsample targets from the FRONT's GetDesc —
//     the swapchain backbuffer at that point) -> W×H;
//   - SetupRenderTargets' cfg -> fields store (ECX/EAX) -> W×H; aspect is
//     re-applied after setup returns (render_outputs' wrap calls
//     apply_policy) because its mutated stretch re-snaps it;
//   - a per-Present invariant check: logs field drift (canary for unknown
//     writers), re-applies aspect/mouse drift (script aspect setters).
// The carrier's output surfaces (render_outputs) are created from the render
// fields, so they follow automatically.
namespace mc2vr::render_size {

// mc2vr.conf render_size=WxH (e.g. 2016x2240) | off (default: stock — render
// size = window size).
bool set_size(const char *value);
// mc2vr.conf render_aspect=true|snap (default true: the real W/H ratio;
// snap = keep the engine's 4:3/16:9 snapping).
bool set_aspect(const char *value);
// mc2vr.conf render_size_follow_hmd=on|off (default on): render_size applies
// only while the HMD is in use (host session VISIBLE/FOCUSED + tracked, 1 s
// debounce); otherwise the game renders at window size, exactly as unmodded.
// A switch runs through the engine's own device-lost recovery.
bool set_follow_hmd(const char *value);

// device.cpp TestCooperativeLevel hook: true (answer D3DERR_DEVICENOTRESET)
// exactly once per armed switch, and only for RenderSystem_CheckDevice's call
// (`ret` = the device method's return address). Applies the pending target.
bool consume_forced_reset(void *ret);

// Attach: the OnScreenResize entry/epilogue and SetupRenderTargets store
// MidHooks (installed when render_size is set or debug_rt_probe is on —
// without render_size they only forward to the probe).
void install();

// After SetupRenderTargets returns (render_outputs' wrap): re-apply aspect.
void after_setup();

// Device Present tap (main thread): invariant check.
void on_present();

void report_window();

} // namespace mc2vr::render_size
