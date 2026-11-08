// Shared-handle image path — carrier side (docs/stereo.md).
//
// At the pass boundaries the carrier-owned front (render_outputs.cpp) holds
// the per-eye tonemapped LDR finals (1->2 = pass-1 LEFT, 2->0 = pass-2
// RIGHT). This module blits them into a ring
// of DEFAULT-pool textures created with legacy pSharedHandle — the mechanism
// PROVEN cross-process by tools/probe/run_shared_handle.sh (2026-10-04: DXVK
// D3D9 -> DXVK D3D11 OpenSharedResource works for both A8R8G8B8 and
// X8R8G8B8, 9Ex and plain devices) — and publishes FRAME_READY to the host
// once the pair is GPU-complete (event query).
//
// Captures run on the render thread (set_pass call sites); FRAME_READY goes
// out once the pair's event query signals (see async_completion below).
// Never blocks on the host. DXVK requires one GetData(D3DGETDATA_FLUSH) or the
// command buffer is never submitted (probe-proven).
//
// Always on. Everything degrades silently: no host -> no capture (the ring
// is never allocated), creation failure -> module off for the run.
#pragma once

#include <cstdint>

namespace mc2vr::eye_share {

// Called from eye_replay::set_pass at the pass boundaries with the live
// device + the carrier front (nullptr when not owned yet — skipped).
// `next_pass`: 2 = boundary 1->2 (capture LEFT), 0 = boundary 2->0 (capture
// RIGHT).
void on_pass_boundary(uint32_t next_pass, void *device, void *source);

// Called from eye_replay::on_reset — all surfaces are gone (the device will
// be recreated; the ring re-creates lazily on the next boundary).
void on_reset();

// 10s window report (called from eye_replay::report_window); resets counters.
void report_window();

// Frame completion: by default each pair's FRAME_READY is sent by a carrier
// thread polling the pair's event query (the device is created with
// D3DCREATE_MULTITHREADED for that), so the render thread never waits for the
// GPU. debug_sync_frame_ready=on restores the synchronous end-of-frame wait
// (and the non-multithreaded device). Must be decided before CreateDevice.
bool set_sync_frame_ready(const char *value);
bool async_completion();

} // namespace mc2vr::eye_share
