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

namespace mc2vr::device {

// mc2vr.conf vsync=on|off (default on — the game's own PresentationInterval
// is untouched). off = vsync pacing: force D3DPRESENT_INTERVAL_IMMEDIATE in
// CreateDevice (via the Direct3DCreate9 thunk hook below) and on every Reset —
// each frame has two Presents (one per draw pass) and both block on a 60 Hz
// vsync slot, capping the game at ~30 Hz (live-log-proven 2026-10-05).
bool set_vsync(const char *value);

// Attach-time: InlineHook the game's Direct3DCreate9 import thunk
// (MC2_D3DCREATE9_THUNK) and VmtHook the returned IDirect3D9. Its CreateDevice
// hook patches the creation params (vsync=off) and installs the device
// VmtHook before the engine sees the device. Must run before the game's
// first instruction (the game is suspended then). Failure = no device hooks
// this run (logged; the game runs unmodified).
bool install_d3d9_gate();

// Full-surface StretchRect through the ORIGINAL device method (bypasses the
// hook chain entirely — used by the eye module's monitor pin to snapshot and
// restore the backbuffer around pass 2; render thread only).
bool blit_surfaces(void *src, void *dst);

} // namespace mc2vr::device
