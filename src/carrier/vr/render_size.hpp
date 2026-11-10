#pragma once

#include <cstdint>
#include <optional>

// Render-size policy (docs/render_resolution.md).
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

// mc2vr.conf render_size=hmd | WxH (e.g. 2016x2240) | off (default: stock —
// render size = window size). hmd = the runtime's recommended per-eye size,
// from the OpenXR session (xr_session.hpp); implies following the HMD.
bool set_size(const char *value);
// xr_session::on_update_tick: the runtime's recommended per-eye size (render_size=hmd).
void on_hmd_view_size(uint32_t w, uint32_t h);
// mc2vr.conf render_aspect=true|snap (default true: the real W/H ratio;
// snap = keep the engine's 4:3/16:9 snapping).
bool set_aspect(const char *value);
// mc2vr.conf render_size_follow_hmd=on|off (default on): render_size applies
// only while the HMD is in use (host session VISIBLE/FOCUSED — the runtime's
// own lifecycle signal, no extra debounce); otherwise the game renders at
// window size, exactly as unmodded.
// A switch runs through the engine's own device-lost recovery.
bool set_follow_hmd(const char *value);

// device.cpp TestCooperativeLevel hook: true (answer D3DERR_DEVICENOTRESET)
// exactly once per armed switch, and only for RenderSystem_CheckDevice's call
// (`ret` = the device method's return address). Applies the pending target.
bool consume_forced_reset(void *ret);

// The HMD is in use: host connected, session VISIBLE/FOCUSED (the runtime's
// own debounced lifecycle signal; not the per-frame tracked flag). Also the
// HUD layer's mode signal (vr/hud.cpp).
bool hmd_in_use();

// render_size is configured (WxH or hmd): hooks/render_size.cpp installs the
// rewrite hooks only then.
bool configured();

// Hook events (hooks/render_size.cpp, main thread). A returned size is
// written back by the hook; nullopt = leave the engine's value.
struct Size {
    uint32_t w, h;
};
// OnScreenResize entry (`self` = this): the size to resize to.
std::optional<Size> on_osr_enter(Size requested, uintptr_t self);
// OnScreenResize epilogue (fields/floats/viewports/mouse scale written):
// aspect policy + mouse scale.
void on_osr_done();
// SetupRenderTargets' cfg -> fields store: the size to store.
std::optional<Size> on_setup_store();
// SetupRenderTargets' main-chain desc (GetDesc succeeded): the size the desc
// should say.
std::optional<Size> on_setup_chain_desc(Size desc);

// After SetupRenderTargets returns (render_outputs' wrap): re-apply aspect.
void after_setup();

// Device Present tap (main thread): invariant check.
void on_present();

void report_window();

} // namespace mc2vr::render_size
