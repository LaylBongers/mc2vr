// Hooks for vr/cull_frustum (HMD cull frustum).
#pragma once

namespace mc2vr::hooks {

// Install the third-person camera-clearance MidHook (always on; inert without
// a tracked HMD). The frustum widening is a direct call in the reimplemented
// view builder (reimpl/render/build_camera_constants.hpp).
void install_cull_frustum();

} // namespace mc2vr::hooks
