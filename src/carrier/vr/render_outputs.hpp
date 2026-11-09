#pragma once

#include <cstdint>

#include <windows.h>
#include <d3d9.h> // types only

// Carrier-owned frame outputs (docs/render_outputs.md).
//
// The engine draws its final frame (gameplay composite via render-command
// op 6, menus/loading via LtiRenderer_EndSubmit) into the "front" surface of
// its render target table — stock = the swapchain backbuffer. The carrier
// creates its own front + depth surfaces and
// re-points every engine holder at them (table +0x3ea4/+0x3e98 front,
// +0x3e94 auto depth, +0x3ea0 when it aliases it, g_Dx9AutoDepthRef), so the
// engine never draws into the swapchain. The swapchain becomes the carrier's
// monitor mirror: before each Present the carrier blits the latest LEFT image
// into it. Eye captures read the carrier front instead of the backbuffer.
//
// Takeover points (main thread): SetupRenderTargets exit (InlineHook wrap,
// boot), RenderSystem_CheckDevice's device-restore tail (MidHook — Reset does
// not re-run SetupRenderTargets) and an every-Present invariant check.
// RE: docs/reverse_engineering/render_target_table.md.
namespace mc2vr::render_outputs {

// Takeover points (hooks/render_outputs.cpp). Always on (it was the
// own_outputs=on option until 2026-11-08; the swapchain-drawing path is
// retired).
// After the original SetupRenderTargets returns: re-apply aspect, take over.
void on_setup_done();
// RenderSystem_CheckDevice, right after the engine's device-restore calls.
void on_device_restored();

// True once the carrier front is installed in the engine's table.
bool active();
// The carrier front (capture source; scene MSAA's sample count), or nullptr
// when not active (boot before setup, mid-Reset).
IDirect3DSurface9 *front();
// The carrier depth (the engine's auto depth and scene-depth alias; same
// sample count), or nullptr when not active.
IDirect3DSurface9 *depth();
// The eye-capture source for this pass boundary: front(), or nullptr during
// the post-Reset transition frame (its HDR exposure is invalid — white).
IDirect3DSurface9 *capture_source();
// The engine's D3D device (g_LtiRenderer->dx9State), or nullptr.
IDirect3DDevice9 *device();
// The resolved LEFT image of the current frame (valid after 1->2), or nullptr.
IDirect3DSurface9 *mirror();
// Resolve the front into a non-MSAA scratch RT and return it (dumps), or nullptr.
IDirect3DSurface9 *resolve_front();

// device.cpp taps (main thread).
void on_present(IDirect3DDevice9 *device);
void on_reset(bool before, long hr);

// eye_replay pass boundaries (next pass: 1 = pass 1 start, 2 = pass 1 done,
// 0 = pass 2 done). 1->2 refreshes the LEFT monitor mirror.
void on_pass_boundary(uint32_t next_pass);

// Duplicate-Present retirement (stereo passes only; stream_capture.cpp).
// Before pass 1: set the engine's g_SuppressPresent so pass 1's present-prev
// is skipped — pass 2's Present (after the 1->2 mirror refresh) shows the
// current LEFT; pass 1's would re-show the previous frame's. The engine
// clears the flag after BeginSubmit; end_present_skip() clears it again
// before pass 2 (BeginSubmit can bail out before its clear).
void begin_present_skip();
void end_present_skip();

void report_window();

} // namespace mc2vr::render_outputs
