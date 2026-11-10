// Eye pair -> swapchain copies on DXVK's queue — see xr_present.hpp.

#include "vr/xr_present.hpp"

#include <windows.h>

#include <initializer_list>
#include <type_traits>

#include "log.hpp"

namespace mc2vr::xr_present {

namespace {

// Device-level entry points we call.
struct Vk {
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkResetCommandBuffer ResetCommandBuffer;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCmdCopyImage CmdCopyImage;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkCreateFence CreateFence;
    PFN_vkWaitForFences WaitForFences;
    PFN_vkResetFences ResetFences;
};
Vk vk = {};

dxvk::ID3D9VkInteropDevice *g_interop = nullptr;
VkDevice g_device = VK_NULL_HANDLE;
VkQueue g_queue = VK_NULL_HANDLE;
VkCommandPool g_pool = VK_NULL_HANDLE;

// One command buffer per copy in flight; reused once its fence signals.
constexpr uint32_t kSlots = 3;
struct Slot {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool in_flight = false;
};
Slot g_slots[kSlots];
uint32_t g_next = 0;
CRITICAL_SECTION g_fence_lock;  // fence waits/resets: the copies vs wait_idle() (any thread)

constexpr uint64_t kFenceTimeoutNs = 100ull * 1000 * 1000;
uint32_t g_fail_logs = 0;

void fail(const char *what, VkResult r)
{
    if (g_fail_logs++ < 8) {
        MC2VR_LOG("xr: present: %s failed (VkResult %d)", what, (int)r);
    }
}

PFN_vkGetInstanceProcAddr find_gipa()
{
    // DXVK loads winevulkan.dll under Wine, else vulkan-1.dll; its handles
    // are only valid through the same library.
    for (const wchar_t *name : {L"winevulkan.dll", L"vulkan-1.dll"}) {
        if (HMODULE m = GetModuleHandleW(name)) {
            if (auto f = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(m, "vkGetInstanceProcAddr"))) {
                MC2VR_LOG("xr: present: Vulkan entry points from %ls", name);
                return f;
            }
        }
    }
    return nullptr;
}

// Waits for slot `s`'s previous copy and makes it reusable (g_fence_lock
// held). Only an error (device loss) gives up; a timeout is logged and waited
// out, since the images it reads may be released right after.
bool reclaim(Slot &s)
{
    if (!s.in_flight) {
        return true;
    }
    VkResult r;
    while ((r = vk.WaitForFences(g_device, 1, &s.fence, VK_TRUE, kFenceTimeoutNs)) == VK_TIMEOUT) {
        fail("vkWaitForFences (still waiting)", r);
    }
    if (r != VK_SUCCESS) {
        fail("vkWaitForFences", r);
        return false;
    }
    vk.ResetFences(g_device, 1, &s.fence);
    s.in_flight = false;
    return true;
}

VkImageMemoryBarrier barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
                             VkAccessFlags dst_access)
{
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return b;
}

} // namespace

bool init(dxvk::ID3D9VkInteropDevice *interop, VkInstance instance, VkDevice device, VkQueue queue,
          uint32_t queue_family)
{
    InitializeCriticalSection(&g_fence_lock);
    g_interop = interop;
    g_device = device;
    g_queue = queue;
    const PFN_vkGetInstanceProcAddr gipa = find_gipa();
    auto gdpa = gipa ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(instance, "vkGetDeviceProcAddr")) : nullptr;
    if (gdpa == nullptr) {
        MC2VR_LOG("xr: present: no vkGetInstanceProcAddr/vkGetDeviceProcAddr in the process — off");
        return false;
    }
    bool ok = true;
    auto load = [&](auto &fn, const char *name) {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(gdpa(device, name));
        if (fn == nullptr) {
            MC2VR_LOG("xr: present: %s missing", name);
            ok = false;
        }
    };
    load(vk.CreateCommandPool, "vkCreateCommandPool");
    load(vk.AllocateCommandBuffers, "vkAllocateCommandBuffers");
    load(vk.ResetCommandBuffer, "vkResetCommandBuffer");
    load(vk.BeginCommandBuffer, "vkBeginCommandBuffer");
    load(vk.EndCommandBuffer, "vkEndCommandBuffer");
    load(vk.CmdPipelineBarrier, "vkCmdPipelineBarrier");
    load(vk.CmdCopyImage, "vkCmdCopyImage");
    load(vk.QueueSubmit, "vkQueueSubmit");
    load(vk.CreateFence, "vkCreateFence");
    load(vk.WaitForFences, "vkWaitForFences");
    load(vk.ResetFences, "vkResetFences");
    if (!ok) {
        return false;
    }

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queue_family;
    VkResult r = vk.CreateCommandPool(device, &pci, nullptr, &g_pool);
    if (r != VK_SUCCESS) {
        fail("vkCreateCommandPool", r);
        return false;
    }
    for (Slot &s : g_slots) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = g_pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if ((r = vk.AllocateCommandBuffers(device, &ai, &s.cmd)) != VK_SUCCESS ||
            (r = vk.CreateFence(device, &fci, nullptr, &s.fence)) != VK_SUCCESS) {
            fail("command buffer / fence creation", r);
            return false;
        }
    }
    MC2VR_LOG("xr: present: command pool on DXVK's device (queue family %u), %u copy slots", queue_family,
              kSlots);
    // Handles as the native side (validation layers) sees them: Wine passes
    // non-dispatchable handles through unchanged (docs/xr_session.md).
    MC2VR_LOG("xr: present: handles: VkCommandPool 0x%llx, VkFence 0x%llx 0x%llx 0x%llx",
              (unsigned long long)g_pool, (unsigned long long)g_slots[0].fence,
              (unsigned long long)g_slots[1].fence, (unsigned long long)g_slots[2].fence);
    return true;
}

bool copy_pair(const CopyImage src[2], const VkImage dst[2], uint32_t w, uint32_t h)
{
    EnterCriticalSection(&g_fence_lock);
    Slot &s = g_slots[g_next];
    const bool free = reclaim(s);
    LeaveCriticalSection(&g_fence_lock);
    if (!free) {
        return false;
    }

    VkResult r = vk.ResetCommandBuffer(s.cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (r != VK_SUCCESS || (r = vk.BeginCommandBuffer(s.cmd, &bi)) != VK_SUCCESS) {
        fail("vkBeginCommandBuffer", r);
        return false;
    }

    // In: the eye images were written by DXVK's submissions on this queue,
    // all earlier in submission order than this one (the render thread
    // flushed them before handing the pair over, and the queue lock drains
    // DXVK's submission queue first). A barrier's first scope covers every
    // earlier-submitted command on the queue, so this one waits for those
    // writes on the GPU and makes them visible. The swapchain images' old
    // contents are discarded.
    VkImageMemoryBarrier pre[4];
    for (int e = 0; e < 2; e++) {
        pre[e] = barrier(src[e].image, src[e].layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        pre[2 + e] = barrier(dst[e], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                             VK_ACCESS_TRANSFER_WRITE_BIT);
    }
    vk.CmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                          nullptr, 0, nullptr, 4, pre);

    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {w, h, 1};
    for (int e = 0; e < 2; e++) {
        vk.CmdCopyImage(s.cmd, src[e].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst[e],
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }

    // Out: the eye images back to the layout DXVK expects, ordered before
    // its later writes to them (the ring reuses them); the swapchain images
    // in the layout OpenXR expects at release, the writes made available.
    VkImageMemoryBarrier post[4];
    for (int e = 0; e < 2; e++) {
        post[e] = barrier(src[e].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, src[e].layout, 0,
                          VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        post[2 + e] = barrier(dst[e], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                              VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    }
    vk.CmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                          nullptr, 0, nullptr, 4, post);

    if ((r = vk.EndCommandBuffer(s.cmd)) != VK_SUCCESS) {
        fail("vkEndCommandBuffer", r);
        return false;
    }

    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s.cmd;
    {
        dxvk::QueueLock lock(g_interop);
        r = vk.QueueSubmit(g_queue, 1, &si, s.fence);
    }
    if (r != VK_SUCCESS) {
        fail("vkQueueSubmit", r);
        return false;
    }
    EnterCriticalSection(&g_fence_lock);
    s.in_flight = true;
    LeaveCriticalSection(&g_fence_lock);
    g_next = (g_next + 1) % kSlots;
    return true;
}

void wait_idle()
{
    if (g_device == VK_NULL_HANDLE || vk.WaitForFences == nullptr) {
        return;
    }
    EnterCriticalSection(&g_fence_lock);
    for (Slot &s : g_slots) {
        reclaim(s);
    }
    LeaveCriticalSection(&g_fence_lock);
}

} // namespace mc2vr::xr_present
