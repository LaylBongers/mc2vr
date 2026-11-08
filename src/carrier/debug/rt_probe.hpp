#pragma once

#include <cstdint>

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

// SetupRenderTargets entry (called by render_outputs' wrapper, main thread).
void on_setup_enter(void *self);
// OnScreenResize entry with the ENGINE's args (render_size's entry MidHook,
// before any rewrite).
void on_osr_enter(uint32_t w, uint32_t h, uintptr_t self, uintptr_t esp);

// Device-hook taps (device.cpp, render thread). `ret` = the device method's
// return address (the immediate caller).
void on_present(void *device);
void on_set_render_target(uint32_t index, void *surface, void *ret);
void on_stretch_rect(void *src, void *dst, void *ret);
void on_reset(bool before, long hr);

enum class Create : uint8_t { Texture, RenderTarget, DepthStencil, OffscreenPlain };
void on_create(Create kind, uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage,
               uint32_t pool, long hr, void *object, void *ret);

enum class Getter : uint8_t { BackBuffer, RenderTarget, DepthStencil, SwapChain };
void on_getter(Getter kind, uint32_t index, void *result, void *ret);

} // namespace mc2vr::rt_probe
