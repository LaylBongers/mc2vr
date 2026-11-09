// Hooks for vr/camera_table (union HMD camera injection at g_CameraTable).
#pragma once

namespace mc2vr::hooks {

// Install the fill-site MidHook (plaintext .text, single caller — the fill
// loop; fires once per filled slot). Handler no-ops unless the conf enabled
// it. Failure is non-fatal: the union injection stays idle.
void install_camera_table();

} // namespace mc2vr::hooks
