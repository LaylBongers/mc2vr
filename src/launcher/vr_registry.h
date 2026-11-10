// The Wine VR registry (docs/openxr_proton.md item 2), owned by the launcher
// because it runs before the game.
//
// wineopenxr negotiation needs HKCU\Software\Wine\VR with DWORD state=1, and
// DXVK (in any process) reads the OpenXR runtime's Vulkan requirements from
// that key when it creates its instance and device: the instance/device
// extension lists and the GPU vendor/product ids, which
// wineopenxr_init_registry publishes. A Steam VR launch normally provides both;
// a standalone launch must do it itself.
//
// wineopenxr_init_registry creates an OpenXR instance in the calling process,
// so the launcher (32-bit) needs SteamVR's 32-bit runtime registered
// (xr_runtime.h, the README setup step). It doesn't touch the process
// environment, so the game inherits nothing from it.
//
// wineopenxr_init_registry returns TRUE even when it publishes nothing, and
// Wine skips writing a value whose data is unchanged without touching the key,
// so neither tells whether this run published. Instead the published values
// are snapshotted and cleared first: whatever is there afterwards was written
// by this run. If they don't come back, the snapshot is restored, so a failure
// never leaves less than before.
#pragma once

#include <windows.h>

typedef void (*VrRegistryLog)(const char *fmt, ...);

// state=1 if absent, then publish and check; logs whether this run published
// the runtime's Vulkan requirements. Under Wine only.
void vr_registry_init(VrRegistryLog log);
