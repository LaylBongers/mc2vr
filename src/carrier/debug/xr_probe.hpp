// debug_xr_probe=on: can OpenXR run inside the game process, on DXVK's own
// Vulkan device? (docs/host.md "OPEN: in-process OpenXR").
//
// Run with the host skipped (MC2VR_NO_HOST=1 ./launch.sh) — two sessions
// would conflict; the probe refuses to run while a host is connected. The
// game needs a 32-bit OpenXR runtime registered (the launcher log says
// whether it is, docs/openxr_proton.md item 4).
//
// Steps, each logged with its result (`xrprobe:` lines), stopping at the
// first failure:
//   1. At attach: a first-chance fault log (the Wine VR registry DXVK reads
//      at device creation is the launcher's, src/launcher/vr_registry.h).
//   2. First Present, on its own thread: DXVK's interop interfaces on the
//      game's device — the Vulkan instance / physical device / device /
//      queue, and the instance extensions DXVK enabled.
//   3. The 32-bit wineopenxr.dll, negotiated directly
//      (xrNegotiateLoaderRuntimeInterface, no loader); an instance with
//      XR_KHR_vulkan_enable; the HMD system; the runtime's Vulkan
//      requirements, its required instance/device extensions (instance ones
//      checked against DXVK's), and the physical device it wants (checked
//      against DXVK's).
//   4. A session on DXVK's device and queue; the session states up to READY;
//      xrBeginSession, a LOCAL space, 180 frames with no layers (wait/begin/
//      end, the frame period measured) and one xrLocateViews; then a clean
//      exit (request exit, end, destroy).
// No swapchains, no rendering: the game keeps drawing its mono frame. The
// runtime gets DXVK's queue; calls that may submit are made under DXVK's
// submission-queue lock.
#pragma once

namespace mc2vr::xr_probe {

// mc2vr.conf debug_xr_probe=on|off (default off). False on bad input.
bool set_enabled(const char *value);

// Carrier attach: step 1.
void install_early();

// The engine's first Present: starts the probe thread (steps 2-4).
void start();

} // namespace mc2vr::xr_probe
