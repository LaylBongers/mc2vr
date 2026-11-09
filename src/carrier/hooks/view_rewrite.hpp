// Hooks for vr/view_rewrite (per-eye camera channel).
#pragma once

namespace mc2vr::hooks {

// Install the upload-gate MidHook. Failure is non-fatal (rewrite stays idle).
void install_view_rewrite();

} // namespace mc2vr::hooks
