// Hooks for vr/cull_frustum (HMD cull frustum).
#pragma once

namespace mc2vr::hooks {

// Install the builder MidHook (installed always; handler gated on the conf)
// and the third-person camera-clearance MidHook.
void install_cull_frustum();

} // namespace mc2vr::hooks
