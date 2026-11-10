// The Vulkan side of presentation (docs/xr_session.md § Presentation): copies
// an eye pair from the game's eye images (DXVK's VkImages behind the D3D9
// ring textures, eye_share.hpp) into the OpenXR swapchain images, with our own
// command buffers submitted on DXVK's queue under its submission lock.
//
// The copy is raw bytes (vkCmdCopyImage): the sRGB-encoded LDR finals land in
// an sRGB swapchain unchanged and the compositor decodes them — no double
// gamma. Swapchains are created at the eye images' own size, so it is 1:1.
//
// Render thread (the stereo frame), except wait_idle().
#pragma once

#include <cstdint>

#include "vr/dxvk_interop.hpp"

namespace mc2vr::xr_present {

// Resolves the Vulkan entry points (from the Vulkan library DXVK loaded) and
// creates the command pool on DXVK's device. False (logged) on failure.
bool init(dxvk::ID3D9VkInteropDevice *interop, VkInstance instance, VkDevice device, VkQueue queue,
          uint32_t queue_family);

struct CopyImage {
    VkImage image;
    VkImageLayout layout;  // the eye image's resting layout (unused for swapchain images)
};

// Records and submits one pair: src[e] -> dst[e] for both eyes, w x h. The
// eye images' writes must already be in DXVK's submission queue (eye_share
// flushes them); the GPU orders the copy after them.
// Swapchain images come back in COLOR_ATTACHMENT_OPTIMAL (the OpenXR Vulkan
// contract at release); the eye images in their own layout. False (logged)
// on failure — nothing was submitted then.
bool copy_pair(const CopyImage src[2], const VkImage dst[2], uint32_t w, uint32_t h);

// Waits until every submitted copy has finished on the GPU.
void wait_idle();

} // namespace mc2vr::xr_present
