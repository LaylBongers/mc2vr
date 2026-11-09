// Device-creation policy: what the carrier changes in the engine's
// CreateDevice / Reset requests (vsync=off interval patch, swapchain MSAA drop,
// D3DCREATE_MULTITHREADED for async frame completion) and the one-shot
// present-parameter log. Plain functions; the IDirect3D9::CreateDevice and
// device Reset/Present hooks (hooks/device.cpp) call them.
#pragma once

#include <windows.h>
#include <d3d9.h> // types only

namespace mc2vr::device_params {

// mc2vr.conf vsync=on|off (default on — the game's own PresentationInterval
// is untouched). off = vsync pacing: force D3DPRESENT_INTERVAL_IMMEDIATE in
// CreateDevice and on every Reset — each frame has two Presents (one per draw
// pass) and both block on a 60 Hz vsync slot, capping the game at ~30 Hz
// (live-log-proven 2026-10-05).
bool set_vsync(const char *value);

// Policy applied to a COPY of the engine's params (`what` = "CreateDevice" /
// "Reset", for the log). The engine keeps its stored copy exactly as it asked.
D3DPRESENT_PARAMETERS apply_param_policy(const D3DPRESENT_PARAMETERS &engine, const char *what);

// Mirror D3D's updates of the params it was given back into the engine's
// struct, keeping the policy fields exactly as the engine requested.
void copy_back_params(D3DPRESENT_PARAMETERS *engine, const D3DPRESENT_PARAMETERS &used);

// CreateDevice behavior flags the carrier needs (logs the game's own flags).
DWORD apply_behavior_policy(DWORD behavior_flags);

// Logs the swapchain's actual present params (main thread; first Present).
void log_present_params(IDirect3DDevice9 *device);

} // namespace mc2vr::device_params
