// Scene MSAA (docs/msaa.md): the 3D scene, the depth prepass and the
// composite render multisampled.
//
//   - The carrier front and depth (vr/render_outputs) are created with the
//     MSAA sample count. The depth is the engine's scene depth (its auto-depth
//     alias); an own engine scene depth (in-game AA on) maps onto it too, since
//     nothing samples the scene depth.
//   - The engine's main color (+0x3e90) and DepthBufferCopy (+0x3e9c) are
//     texture levels, which cannot be multisampled: each gets an MSAA
//     stand-in surface. When the engine binds one as RT0, the device gets
//     the stand-in; the stand-in is resolved into the real surface before
//     the real one is read (StretchRect source, its texture bound to a
//     sampler) and when it is unbound while its texture sits on a sampler.
//   - The depth follows the bound RT0: the MSAA depth is only bound with an
//     MSAA RT0, a 1x RT0 gets no depth (only draw-less binds in the engine).
//     The mapping is the same for every bind, so the game's bind caches
//     (logical pointers) stay valid.
//
// Device-hook entry points are render thread only.
#pragma once

#include <windows.h>
#include <d3d9.h> // types only

#include <cstdint>

namespace mc2vr::msaa {

// conf `msaa=off|2|4|8` (default 4).
bool set_samples(const char *value);

// The sample type for the carrier front/depth: the configured count when the
// device supports it for every surface involved (checked once, at the first
// call with a device), else NONE.
D3DMULTISAMPLE_TYPE outputs_type(IDirect3DDevice9 *device, D3DFORMAT front_format, D3DFORMAT depth_format);

// Lifecycle (vr/render_outputs): create or refresh the stand-ins after a
// takeover and at every Present (cheap when nothing changed); release them
// before Reset.
void ensure(IDirect3DDevice9 *device);
void on_reset_before();

// Device hooks (hooks/device.cpp). The returned surfaces are what the device
// gets.
IDirect3DSurface9 *map_render_target(DWORD index, IDirect3DSurface9 *logical);
IDirect3DSurface9 *map_depth(IDirect3DSurface9 *logical);
// After an RT0 change: true with the depth to bind when it differs from the
// bound one.
bool depth_after_rt0(IDirect3DSurface9 **physical);
void before_stretch_rect(IDirect3DSurface9 *src, IDirect3DSurface9 *dst);
void before_set_texture(DWORD stage, IDirect3DBaseTexture9 *texture);
// Device Reset: the device's bindings are back to its defaults.
void on_reset_done();

// LtiRenderer_BeginSubmit (each pass): multisample rasterization on. The
// engine sets D3DRS_MULTISAMPLEANTIALIAS = (its own AA samples != 0) at device
// setup and restore (0x0074c860, 0x0074d099); with the in-game AA off that is
// FALSE, and MSAA targets then get identical coverage in every sample.
// Through Dx9_SetRenderState, so the engine's render-state cache agrees.
void on_begin_submit();

// 10 s window report (render_dump.cpp poller); resets counters.
void report_window();

} // namespace mc2vr::msaa
