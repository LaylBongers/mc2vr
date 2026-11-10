// Eye images: the hand-off from the render thread to the OpenXR session
// thread (docs/eye_images.md).
//
// At the pass boundaries the carrier-owned front (render_outputs.cpp) holds
// the per-eye tonemapped LDR finals (1->2 = pass-1 LEFT, 2->0 = pass-2 RIGHT).
// They are blitted into a small ring of D3D9 textures; after the RIGHT blit
// the pair is fenced with an event query and flushed to the GPU, and the
// session thread takes it once the query signals, then copies it into the
// swapchains on DXVK's queue through the VkImages behind the textures
// (xr_present.hpp). The render thread never waits for the GPU.
//
// The ring textures are created with a legacy pSharedHandle although nothing
// opens the handle: DXVK never relocates a shared image, so the VkImage
// behind each texture stays the same for its lifetime.
//
// Captures run only while the session shows frames (xr_session::presenting()).
// Everything degrades silently: creation failure -> module off until the next
// Reset.
#pragma once

#include <cstdint>

#include <windows.h>
#include <d3d9.h> // types only

#include "vr/xr_present.hpp"

namespace mc2vr::eye_share {

// Called from eye_replay::set_pass at the pass boundaries with the live
// device + the carrier front (nullptr when not owned yet — skipped).
// `next_pass`: 2 = boundary 1->2 (capture LEFT), 0 = boundary 2->0 (capture
// RIGHT).
void on_pass_boundary(uint32_t next_pass, IDirect3DDevice9 *device,
                      IDirect3DSurface9 *source);

// Called from eye_replay::on_reset — waits until the session thread no longer
// uses the ring (CPU and GPU), then drops it (re-created lazily).
void on_reset();

// 10s window report (called from eye_replay::report_window); resets counters.
void report_window();

// ---- session thread ---------------------------------------------------------

// A complete pair, GPU-done, owned by the session thread until release_pair().
struct Pair {
    uint32_t slot = 0;
    uint64_t frame_id = 0;
    uint32_t pose_id = 0;   // xr_session pose id both eyes rendered with (0 = none)
    uint32_t w = 0, h = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    IDirect3DDevice9 *device = nullptr;  // the device the images belong to
    xr_present::CopyImage eye[2] = {};
};

enum class Take {
    None,   // no new pair in time
    Fresh,  // the pair rendered from `want_pose` (or a later one)
    Late,   // the wait ran out: the newest pair, rendered from an older pose
};

// Waits at most timeout_ms for a pair newer than `after_frame` rendered from
// pose `want_pose` or later (pose-less pairs count as ready), then for its
// GPU work. Older unclaimed pairs are dropped.
Take take_pair(uint64_t after_frame, uint32_t want_pose, uint32_t timeout_ms, Pair *out);

// The copy of `p` was submitted: its slot may be rewritten.
void release_pair(const Pair &p);

} // namespace mc2vr::eye_share
