// M2: D3D9 device capture + VmtHook pinning of Present/BeginScene/EndScene/
// Reset (docs/launcher_plan.md hook list; docs/reverse_engineering/render_path.md open items:
// Present/EndScene call-site pinning, thunk_FUN_0256b6f0 confirmation).
//
// Discipline notes:
//   - Capture is a direct CALL of the GetD3DDevice thunk (supersedes the
//     plan's original "InlineHook the thunk": capture runs in carrier
//     stage 2 once the device exists (init.cpp), and the thunk is hot-path with 12
//     constant callers — calling is proven safe by probe (b)).
//   - VmtHook clones the object's vtable and swaps the vptr — the original
//     DXVK vtable in d3d9.dll is never touched. The swap is an aligned
//     4-byte store; a concurrent in-flight virtual call keeps using the
//     valid old vtable, so no thread suspension is needed.
//   - Present params are queried from the MAIN thread (first Present hook
//     call), not the init thread — D3D9 device use is main-thread-only
//     (docs/reverse_engineering/render_path.md threading model).
//   - Hook objects are deliberately leaked (heap, never destroyed): their
//     destructors would restore the object's vptr during process teardown,
//     possibly after DXVK has freed the device (UAF write on exit).
#pragma once

namespace mc2vr::device {

// mc2vr.conf vsync=on|off (default on — the game's own PresentationInterval
// is untouched). off = S4-5 pacing: force D3DPRESENT_INTERVAL_IMMEDIATE in
// CreateDevice (via the Direct3DCreate9 thunk hook below) and on every Reset —
// each frame has two Presents (one per draw pass) and both block on a 60 Hz
// vsync slot, capping the game at ~30 Hz (live-log-proven 2026-10-05).
bool set_vsync(const char *value);

// Stage 1: InlineHook the game's Direct3DCreate9 import thunk
// (MC2_D3DCREATE9_THUNK) and VmtHook the returned IDirect3D9 so CreateDevice
// params can be patched before the device exists. No-op when vsync=on. Must
// run before the game's first instruction (the game is suspended then).
bool install_d3d9_gate();

// Capture the device, log present parameters (from the first Present call),
// and install the vtable hooks. Returns false on capture/install failure
// (the game keeps running either way; failures are logged).
bool capture_and_hook();

// Full-surface StretchRect through the ORIGINAL device method (bypasses the
// hook chain entirely — used by the eye module's monitor pin to snapshot and
// restore the backbuffer around pass 2; render thread only).
bool blit_surfaces(void *src, void *dst);

} // namespace mc2vr::device
