#pragma once

#include <cstdint>

#include <windows.h>
#include <d3d9.h> // types only

// Render-target / render-size instrumentation
// (docs/reverse_engineering/render_target_table.md, rt_dims_source.md).
// LOG-ONLY: every handler observes and forwards; nothing is altered.
// mc2vr.conf: debug_rt_probe=on (off by default; when off, no extra hooks are
// installed and every tap returns immediately).
namespace mc2vr::rt_probe {

bool set_enabled(const char *value);
bool enabled();

// At attach (plaintext MidHooks, installed while the game is still suspended so
// the boot-time OnScreenResize / SetupRenderTargets calls are seen).
void install_early();

// From hooks/device.cpp hook_device: the creation/getter census slots
// (device::hook_method). No-op unless debug_rt_probe=on.
void install_device_hooks(IDirect3DDevice9 *device);

// SetupRenderTargets entry (called by render_outputs' wrapper, main thread).
void on_setup_enter(void *self);
// OnScreenResize entry with the ENGINE's args (render_size's entry MidHook,
// before any rewrite).
void on_osr_enter(uint32_t w, uint32_t h, uintptr_t self, uintptr_t esp);

// Device-hook taps (hooks/device.cpp, render thread). `ret` = the device
// method's return address (the immediate caller).
void on_present(IDirect3DDevice9 *device);
void on_set_render_target(uint32_t index, IDirect3DSurface9 *surface, void *ret);
void on_stretch_rect(IDirect3DSurface9 *src, IDirect3DSurface9 *dst, void *ret);
void on_reset(bool before, long hr);

} // namespace mc2vr::rt_probe
