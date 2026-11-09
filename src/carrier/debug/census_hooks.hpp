// Debug-only device slots for debug_gpu_census / debug_ps_census, installed
// via device::hook_method at device creation: SetPixelShader (ps_census) and
// the four Draw*Primitive* methods (both). Observe + forward only.
#pragma once

#include <windows.h>
#include <d3d9.h> // types only

namespace mc2vr::census_hooks {

// From hooks/device.cpp hook_device (main thread, the engine has not seen the
// device yet). No-op unless debug_gpu_census or debug_ps_census is on.
void install(IDirect3DDevice9 *device);

} // namespace mc2vr::census_hooks
