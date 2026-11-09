// Verification of reimplementations against their originals
// (debug_reimpl=shadow:<Ghidra name>; docs/reimpl.md). The originals keep
// running. One function at a time: the verifiers hook some of the same
// callee entries.
//   RenderCmd_ExecuteStream  dry run alongside each call (harness.hpp, render_cmd.cpp)
//   PgPrimitive_SubmitToGPU  paired stereo passes, original vs ours (submit_to_gpu.cpp)
#pragma once

namespace mc2vr::shadow {

// Installs the named function's verifier; false (logged) if unknown or failed.
bool install(const char *name);

bool install_render_cmd();
bool install_submit_to_gpu();

} // namespace mc2vr::shadow
