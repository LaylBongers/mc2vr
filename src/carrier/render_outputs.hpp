#pragma once

#include <cstdint>

// Carrier-owned frame outputs (docs/render_resolution.md §2 / track 3).
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

// Attach: InlineHook-wrap PgLtiRendererPc_SetupRenderTargets + the
// CheckDevice device-restore MidHook. Always on (it was the own_outputs=on
// option until 2026-11-08; the swapchain-drawing path is retired).
void install();

// True once the carrier front is installed in the engine's table.
bool active();
// The carrier front (capture source; MSAA as the engine requested), or
// nullptr when not active (boot before setup, mid-Reset).
void *front();
// The engine's D3D device (g_LtiRenderer->dx9State), or nullptr.
void *device();
// The resolved LEFT image of the current frame (valid after 1->2), or nullptr.
void *mirror();
// Resolve the front into a non-MSAA scratch RT and return it (dumps), or nullptr.
void *resolve_front();

// CreateDevice/Reset parameter policy (device.cpp): the MSAA the engine asked
// for (applied to the carrier front/depth) — the swapchain itself runs
// without MSAA.
void note_requested_msaa(uint32_t type, uint32_t quality);

// device.cpp taps (main thread).
void on_present(void *device);
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
