// Hooks for vr/cull_frustum (HMD cull frustum).
#pragma once

namespace mc2vr::hooks {

// Install the builder MidHook and the third-person camera-clearance MidHook
// (both always on; inert without a tracked HMD).
void install_cull_frustum();

} // namespace mc2vr::hooks
