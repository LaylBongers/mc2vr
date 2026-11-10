// Eye images: the stereo frame's two LDR finals, as Vulkan images for the
// OpenXR copy (docs/eye_images.md).
//
// At the pass boundaries the carrier-owned front (render_outputs.cpp) holds
// the per-eye tonemapped LDR finals (1->2 = pass-1 LEFT, 2->0 = pass-2 RIGHT).
// They are blitted into one pair of D3D9 textures; after the RIGHT blit the
// pair's work is flushed into DXVK's submission queue (FlushRenderingCommands),
// and the frame's end copies it into the OpenXR swapchains on DXVK's queue
// through the VkImages behind the textures (xr_session::end_frame,
// xr_present.hpp). Our copy is submitted later on the same VkQueue, so the GPU
// orders it after the writes, and the next frame's writes after the copy: no
// thread waits for the GPU, and one pair is enough.
//
// The textures are created with a legacy pSharedHandle that nothing opens:
// DXVK never relocates a shared image, so the VkImage behind each texture
// stays the same for its lifetime.
//
// Render thread only (the stereo frame in reimpl/render/submit_to_gpu.cpp).
// Everything degrades silently: creation failure -> off until the next Reset.
#pragma once

#include <cstdint>

#include <windows.h>
#include <d3d9.h> // types only

#include "vr/xr_present.hpp"

namespace mc2vr::eye_share {

// The frame's eye pair, ready for the copy (its writes are in DXVK's
// submission queue).
struct Pair {
    uint32_t w = 0, h = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    IDirect3DDevice9 *device = nullptr;  // the device the images belong to
    xr_present::CopyImage eye[2] = {};
};

// Blits `source` (the carrier front) into eye `eye`'s texture (0 = LEFT at the
// 1->2 boundary, 1 = RIGHT at 2->0). False when nothing was captured.
bool capture(uint32_t eye, IDirect3DDevice9 *device, IDirect3DSurface9 *source);

// After both captures of a frame: flushes their work into DXVK's submission
// queue and describes the pair. False when the frame has no complete pair.
bool finish(Pair *out);

// Called from eye_replay::on_reset — waits for our GPU copies, then drops the
// textures (re-created lazily).
void on_reset();

// 10s window report (called from eye_replay::report_window); resets counters.
void report_window();

} // namespace mc2vr::eye_share
