#include "vr/eye_share.hpp"

#include <windows.h>
#include <d3d9.h>

#include <cstdint>
#include <cstdio>

#include "debug/gpu_census.hpp"
#include "hooks/device.hpp"
#include "log.hpp"
#include "vr/dxvk_interop.hpp"
#include "qpc.hpp"
#include "vr/xr_session.hpp"

namespace mc2vr::eye_share {

namespace {

IDirect3DTexture9 *g_tex[2] = {};           // owned
IDirect3DSurface9 *g_surf[2] = {};          // level 0 (owned ref)
dxvk::ID3D9VkInteropTexture *g_vk[2] = {};  // owned
dxvk::ID3D9VkInteropDevice *g_interop = nullptr;  // owned
bool g_ok = false;      // textures created
bool g_broken = false;  // creation failed — off until the next Reset
IDirect3DDevice9 *g_device = nullptr;
uint32_t g_w = 0, g_h = 0;
VkFormat g_format = VK_FORMAT_UNDEFINED;
bool g_captured[2] = {};  // this frame's captures

// Window counters.
uint64_t g_captures[2] = {}, g_pairs = 0;
double g_flush_sum_ms = 0.0, g_flush_max_ms = 0.0;

void release_resources()
{
    for (int e = 0; e < 2; e++) {
        if (g_vk[e]) g_vk[e]->Release();
        if (g_surf[e]) g_surf[e]->Release();
        if (g_tex[e]) g_tex[e]->Release();
        g_vk[e] = nullptr;
        g_surf[e] = nullptr;
        g_tex[e] = nullptr;
    }
    if (g_interop) g_interop->Release();
    g_interop = nullptr;
    g_ok = false;
}

bool create_resources(IDirect3DDevice9 *device, IDirect3DSurface9 *source)
{
    D3DSURFACE_DESC desc = {};
    if (FAILED(source->GetDesc(&desc))) {
        MC2VR_LOG("share: front GetDesc failed — eye images off");
        return false;
    }
    for (int e = 0; e < 2; e++) {
        // RENDERTARGET: the captures are StretchRect blits. The shared handle
        // keeps the VkImage fixed (eye_share.hpp); it is not used.
        HANDLE shared = nullptr;
        const HRESULT hr = device->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format,
                                                 D3DPOOL_DEFAULT, &g_tex[e], &shared);
        if (FAILED(hr) || g_tex[e] == nullptr) {
            MC2VR_LOG("share: CreateTexture(%ux%u fmt %u, shared RT) FAILED hr=%08lx — eye images off. "
                      "mc2vr needs DXVK's d3d9.dll (docs/platforms.md)",
                      desc.Width, desc.Height, (unsigned)desc.Format, (unsigned long)hr);
            return false;
        }
        if (FAILED(g_tex[e]->GetSurfaceLevel(0, &g_surf[e])) ||
            FAILED(g_tex[e]->QueryInterface(dxvk::kIID_InteropTexture, reinterpret_cast<void **>(&g_vk[e]))) ||
            g_vk[e] == nullptr) {
            MC2VR_LOG("share: surface / ID3D9VkInteropTexture unavailable — eye images off (not DXVK?)");
            return false;
        }
    }
    if (FAILED(device->QueryInterface(dxvk::kIID_InteropDevice, reinterpret_cast<void **>(&g_interop))) ||
        g_interop == nullptr) {
        MC2VR_LOG("share: the device has no ID3D9VkInteropDevice — eye images off (not DXVK?)");
        return false;
    }
    VkImage img[2] = {};
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    if (FAILED(g_vk[0]->GetVulkanImageInfo(&img[0], &layout, &info)) || img[0] == VK_NULL_HANDLE ||
        FAILED(g_vk[1]->GetVulkanImageInfo(&img[1], nullptr, nullptr)) || img[1] == VK_NULL_HANDLE) {
        MC2VR_LOG("share: GetVulkanImageInfo failed — eye images off");
        return false;
    }
    g_device = device;
    g_w = desc.Width;
    g_h = desc.Height;
    g_format = info.format;
    MC2VR_LOG("share: eye images ready: %ux%u, D3DFMT %u = VkFormat %d, layout %d, usage 0x%x, "
              "VkImage L 0x%llx R 0x%llx",
              g_w, g_h, (unsigned)desc.Format, (int)info.format, (int)layout, (unsigned)info.usage,
              (unsigned long long)img[0], (unsigned long long)img[1]);
    return true;
}

bool ensure_resources(IDirect3DDevice9 *device, IDirect3DSurface9 *source)
{
    if (g_ok) {
        return true;
    }
    if (g_broken || device == nullptr || source == nullptr) {
        return false;
    }
    if (!create_resources(device, source)) {
        release_resources();
        g_broken = true;
        return false;
    }
    g_ok = true;
    return true;
}

} // namespace

bool capture(uint32_t eye, IDirect3DDevice9 *device, IDirect3DSurface9 *source)
{
    if (eye == 0) {
        g_captured[0] = g_captured[1] = false;
    }
    // No source: the front isn't owned (boot, mid-Reset) or it's the post-Reset
    // transition frame. A lone RIGHT can't complete a pair.
    if (eye > 1 || source == nullptr || (eye == 1 && !g_captured[0]) || !ensure_resources(device, source)) {
        return false;
    }
    gpu_census::carrier_begin();
    const bool blitted = device::blit_surfaces(source, g_surf[eye]);
    gpu_census::carrier_end();
    g_captured[eye] = blitted;
    g_captures[eye] += blitted ? 1 : 0;
    return blitted;
}

bool finish(Pair *out)
{
    if (!g_ok || !g_captured[0] || !g_captured[1]) {
        return false;
    }
    g_captured[0] = g_captured[1] = false;

    // Hand the pair's work to DXVK's submission queue now: the copy, submitted
    // after this under the queue lock (which first drains that queue), is
    // later in the VkQueue's submission order, so its barrier alone orders it
    // after the eye images' writes, on the GPU. The device lock keeps other
    // D3D9 threads out of the flush (FlushRenderingCommands takes none).
    const int64_t f0 = qpc::now();
    g_interop->LockDevice();
    g_interop->FlushRenderingCommands();
    g_interop->UnlockDevice();
    const double ms = qpc::ms_since(f0);
    g_flush_sum_ms += ms;
    g_flush_max_ms = ms > g_flush_max_ms ? ms : g_flush_max_ms;

    for (int e = 0; e < 2; e++) {
        if (FAILED(g_vk[e]->GetVulkanImageInfo(&out->eye[e].image, &out->eye[e].layout, nullptr)) ||
            out->eye[e].image == VK_NULL_HANDLE) {
            return false;
        }
    }
    out->w = g_w;
    out->h = g_h;
    out->format = g_format;
    out->device = g_device;
    g_pairs++;
    return true;
}

void on_reset()
{
    // Our copies read these images and DXVK doesn't know: let them finish
    // before the textures go.
    xr_session::wait_gpu_copies();
    release_resources();
    g_captured[0] = g_captured[1] = false;
    g_broken = false;  // retry after the Reset
    MC2VR_LOG("share: Reset — eye images dropped");
}

void report_window()
{
    if (g_captures[0] || g_captures[1] || g_pairs) {
        MC2VR_LOG("share window: capturesL=%llu capturesR=%llu pairs=%llu | flush avg=%.2fms max=%.2fms",
                  (unsigned long long)g_captures[0], (unsigned long long)g_captures[1],
                  (unsigned long long)g_pairs, g_pairs ? g_flush_sum_ms / (double)g_pairs : 0.0, g_flush_max_ms);
    }
    g_captures[0] = g_captures[1] = g_pairs = 0;
    g_flush_sum_ms = g_flush_max_ms = 0.0;
}

} // namespace mc2vr::eye_share
