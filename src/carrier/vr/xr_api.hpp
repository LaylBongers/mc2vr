// OpenXR with the Vulkan graphics binding (headers only: the carrier has no
// OpenXR loader, it negotiates with the runtime DLL itself — xr_runtime.hpp).
#pragma once

#include <windows.h>

#include <vulkan/vulkan_core.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>
