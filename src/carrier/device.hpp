// M2: D3D9 device capture + VmtHook pinning of Present/BeginScene/EndScene/
// Reset (docs/launcher_plan.md hook list; render_path.md open items:
// Present/EndScene call-site pinning, thunk_FUN_0256b6f0 confirmation).
//
// Discipline notes:
//   - Capture is a direct CALL of the GetD3DDevice thunk (supersedes the
//     plan's original "InlineHook the thunk": the device pre-exists at
//     carrier-init per the M1 finding, and the thunk is hot-path with 12
//     constant callers — calling is proven safe by probe (b)).
//   - VmtHook clones the object's vtable and swaps the vptr — the original
//     DXVK vtable in d3d9.dll is never touched. The swap is an aligned
//     4-byte store; a concurrent in-flight virtual call keeps using the
//     valid old vtable, so no thread suspension is needed.
//   - Present params are queried from the MAIN thread (first Present hook
//     call), not the init thread — D3D9 device use is main-thread-only
//     (render_path.md threading model).
//   - Hook objects are deliberately leaked (heap, never destroyed): their
//     destructors would restore the object's vptr during process teardown,
//     possibly after DXVK has freed the device (UAF write on exit).
#pragma once

namespace mc2vr::device {

// Capture the device, log present parameters (from the first Present call),
// and install the vtable hooks. Returns false on capture/install failure
// (the game keeps running either way; failures are logged).
bool capture_and_hook();

} // namespace mc2vr::device
