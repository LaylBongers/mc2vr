// The Wine VR registry (docs/openxr_proton.md item 2), owned by the launcher
// because it runs before both the host and the game.
//
// wineopenxr negotiation needs HKCU\Software\Wine\VR with DWORD state=1, and
// DXVK (in any process) reads the OpenXR runtime's Vulkan requirements from
// that key when it creates its instance and device: the instance/device
// extension lists and the GPU vendor/product ids, which
// wineopenxr_init_registry publishes. A Steam VR launch normally provides both;
// a standalone launch must do it itself.
//
// wineopenxr_init_registry creates an OpenXR instance in the calling process,
// so that process must find a runtime. A 64-bit one always does (the system
// registers SteamVR's 64-bit runtime; a 32-bit process may not find one,
// xr_runtime.h), so the launcher runs it through the 64-bit host binary's
// helper mode (`mc2vr_host.exe --vr-registry-init`). Wine gives a child the
// parent's Unix environment, not the environment block passed to
// CreateProcess, so a 32-bit child can't be handed a runtime selection.
#pragma once

#include <windows.h>

typedef void (*VrRegistryLog)(const char *fmt, ...);

// state=1 if absent, then the host helper; logs whether the published values
// were refreshed by this run. Under Wine only.
void vr_registry_init(const wchar_t *host_exe, VrRegistryLog log);
