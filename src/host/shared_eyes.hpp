// S4-2: host side of the shared-handle image path (docs/plans/stereo_design.md §S4). The carrier blits the per-eye LDR finals into
// D3D9 shared-handle textures at the pass boundaries and publishes
// FRAME_READY; this module opens them with ID3D11Device::OpenSharedResource
// (the mechanism PROVEN cross-process by tools/probe/run_shared_handle.sh,
// 2026-10-04) and mirrors the newest pair to a desktop window — the S4-2
// verification step BEFORE any OpenXR submission (S4-3).
//
// The mirror is GDI-side (StretchDIBits from a staging read): no shaders, no
// swapchain, nothing that could interfere with the OpenXR session device.
// All entry points are safe no-ops before init() or on any failure — the
// OpenXR loop never depends on this module.
#pragma once

#include <cstdint>

#include "d3d.hpp"

namespace seyes {

// Create the mirror window + prepare the open cache. False (logged) when the
// window can't be created; the module stays inert afterwards.
bool init(d3d::Device* d);

// MC2VR_CMD_CONFIG from the carrier (ring geometry announcement).
void on_config(uint32_t width, uint32_t height, uint32_t format);

// MC2VR_CMD_FRAME_READY: {frameId, handle, slot, eye, w, h} — opens the handle
// (cached) and remembers it as the newest image for that eye.
void on_frame_ready(uint64_t frameId, uint64_t handle, uint32_t slot,
                    uint32_t eye, uint32_t w, uint32_t h, uint32_t poseId = 0);

// Per host-frame: pump window messages and redraw panes when new frames
// arrived. Cheap when idle.
void pump();

// S4-3 seam: the newest carrier image per eye. Returns true when eye has a
// shared texture opened (with its UNORM-cast SRV). Does NOT consume the
// mirror's `fresh` flag — the OpenXR loop re-submits the newest pair at HMD
// cadence while the game runs ~30 Hz (S4-3 pacing), so freshness is
// irrelevant to the caller.
struct LatestImage {
    ID3D11ShaderResourceView* srv = nullptr;
    uint64_t frameId = 0;
    uint32_t poseId = 0;  // S4-4: carrier pose id (0 = static-pan render)
    uint32_t w = 0, h = 0;
};
bool latest(uint32_t eye, LatestImage& out);

}  // namespace seyes
