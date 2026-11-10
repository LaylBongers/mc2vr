// DXVK's D3D9 interop interfaces (DXVK src/d3d9/d3d9_interfaces.h): the
// Vulkan objects behind the game's D3D9 device and textures. mc2vr requires
// DXVK's d3d9.dll (docs/platforms.md); without it QueryInterface fails and VR
// stays off.
//
// Declared with external linkage on purpose: with internal linkage GCC sees
// no concrete subclass of these abstract interfaces and compiles virtual
// calls through them as unreachable (it emitted `call 0`; a 2026-11-10 probe
// run faulted executing data).
#pragma once

#include <windows.h>
#include <d3d9.h>

#include <vulkan/vulkan_core.h>

namespace mc2vr::dxvk {

struct ID3D9VkInteropInterface : public IUnknown {
    virtual void STDMETHODCALLTYPE GetInstanceHandle(VkInstance *instance) = 0;
    virtual void STDMETHODCALLTYPE GetPhysicalDeviceHandle(UINT adapter, VkPhysicalDevice *phys) = 0;
};

// The image behind a D3D9 texture. `layout` = the layout the image rests in
// between DXVK's commands.
struct ID3D9VkInteropTexture : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetVulkanImageInfo(VkImage *image, VkImageLayout *layout,
                                                         VkImageCreateInfo *info) = 0;
};

// Every method up to the last one used, in vtable order.
struct ID3D9VkInteropDevice : public IUnknown {
    virtual void STDMETHODCALLTYPE GetVulkanHandles(VkInstance *instance, VkPhysicalDevice *phys,
                                                    VkDevice *device) = 0;
    virtual void STDMETHODCALLTYPE GetSubmissionQueue(VkQueue *queue, uint32_t *index,
                                                      uint32_t *family) = 0;
    virtual void STDMETHODCALLTYPE TransitionTextureLayout(ID3D9VkInteropTexture *texture,
                                                           const VkImageSubresourceRange *subresources,
                                                           VkImageLayout old_layout,
                                                           VkImageLayout new_layout) = 0;
    // Not thread-safe against the D3D9 thread (no device lock in DXVK).
    virtual void STDMETHODCALLTYPE FlushRenderingCommands() = 0;
    // Waits until DXVK has handed every queued submission to Vulkan, then
    // holds the queue. No D3D9 calls from the locking thread meanwhile.
    virtual void STDMETHODCALLTYPE LockSubmissionQueue() = 0;
    virtual void STDMETHODCALLTYPE ReleaseSubmissionQueue() = 0;
};

inline constexpr GUID kIID_InteropTexture = {0xd56344f5, 0x8d35, 0x46fd,
                                             {0x80, 0x6d, 0x94, 0xc3, 0x51, 0xb4, 0x72, 0xc1}};
inline constexpr GUID kIID_InteropDevice = {0x2eaa4b89, 0x0107, 0x4bdb,
                                            {0x87, 0xf7, 0x0f, 0x54, 0x1c, 0x49, 0x3c, 0xe0}};

// Holds DXVK's submission queue for the scope (calls the runtime and our
// Vulkan submits make on DXVK's queue go under it).
class QueueLock {
public:
    explicit QueueLock(ID3D9VkInteropDevice *interop) : m_interop(interop) { m_interop->LockSubmissionQueue(); }
    ~QueueLock() { m_interop->ReleaseSubmissionQueue(); }
    QueueLock(const QueueLock &) = delete;
    QueueLock &operator=(const QueueLock &) = delete;

private:
    ID3D9VkInteropDevice *m_interop;
};

} // namespace mc2vr::dxvk
