// D3D9 device capture + VmtHook pinning of Present/BeginScene/EndScene/
// Reset (docs/hooks.md hook list; docs/reverse_engineering/frame_chain.md:
// Present/EndScene call-site pinning, thunk_FUN_0256b6f0 confirmation).
//
// Discipline notes:
//   - Capture happens at the device's birth: the Direct3DCreate9 thunk hook
//     (installed at attach, game suspended) VmtHooks the IDirect3D9, whose
//     CreateDevice hook VmtHooks the new device before returning it to the
//     engine. No polling, no GetD3DDevice thunk call, no cross-thread install.
//   - VmtHook clones the object's vtable and swaps the vptr — the original
//     DXVK vtable in d3d9.dll is never touched. The swap is an aligned
//     4-byte store; a concurrent in-flight virtual call keeps using the
//     valid old vtable, so no thread suspension is needed.
//   - Present params are queried from the MAIN thread (first Present hook
//     call) — D3D9 device use is main-thread-only
//     (docs/reverse_engineering/render_threading.md). The first Present also
//     fires mc2vr::on_engine_running() (init.cpp).
//   - Hook objects are deliberately leaked (heap, never destroyed): their
//     destructors would restore the object's vptr during process teardown,
//     possibly after DXVK has freed the device (UAF write on exit).
#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdint>

namespace safetyhook {
class VmHook;
}

namespace mc2vr::device {

// Attach-time: InlineHook the game's Direct3DCreate9 import thunk
// (MC2_D3DCREATE9_THUNK) and VmtHook the returned IDirect3D9. Its CreateDevice
// hook applies the creation-param policy (vr/device_params.hpp) and installs the device
// VmtHook before the engine sees the device. Must run before the game's
// first instruction (the game is suspended then). Failure = no device hooks
// this run (logged; the game runs unmodified).
bool install_d3d9_gate();

// Full-surface StretchRect through the ORIGINAL device method (bypasses the
// hook chain entirely — the carrier's own captures/resolves; render thread
// only).
bool blit_surfaces(void *src, void *dst);

// StretchRect with explicit rects/filter through the ORIGINAL device method
// (render thread only). Used by render_outputs' monitor mirror.
bool stretch_surfaces(void *src, const RECT *src_rect, void *dst, const RECT *dst_rect,
                      uint32_t filter);

// ---- Debug-only slot hooks ---------------------------------------------------
// Hooks device vtable `slot` on the current device's cloned vtable. For debug
// tools' own census slots (rt_probe, census_hooks), called from hook_device;
// never for a slot the core already hooks. Returns the leaked VmHook (call the original through it),
// or nullptr with `*error` = the SafetyHook error type.
safetyhook::VmHook *hook_method(size_t slot, void *destination, unsigned *error = nullptr);

} // namespace mc2vr::device
