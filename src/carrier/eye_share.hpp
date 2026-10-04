// S4-2: shared-handle image path — carrier side (docs/stereo_design.md §S4,
// docs/s4_handover.md S4-2).
//
// At the pass boundaries the backbuffer holds the per-eye tonemapped LDR
// finals (1->2 = pass-1 LEFT, 2->0 = pass-2 RIGHT before the monitor-pin
// restore — the S4-2 design observation). This module blits them into a ring
// of DEFAULT-pool textures created with legacy pSharedHandle — the mechanism
// PROVEN cross-process by tools/probe/run_shared_handle.sh (2026-10-04: DXVK
// D3D9 -> DXVK D3D11 OpenSharedResource works for both A8R8G8B8 and
// X8R8G8B8, 9Ex and plain devices) — and publishes FRAME_READY to the host
// after an event-query GPU sync.
//
// Rules honored: render thread only (set_pass call sites); never blocks on
// the host (ring push is lock-free, drop+count when full); GPU-sync spin is
// bounded (the next BeginSubmit waits for the same work anyway — the wait is
// front-loaded, not added); DXVK requires D3DGETDATA_FLUSH on GetData or the
// command buffer is never submitted (probe-proven — without it the copies
// never land).
//
// eye_share=off|on. Everything degrades silently: no host -> no publish
// (blits still happen; cheap), creation failure -> module off for the run.
#pragma once

#include <cstdint>

namespace mc2vr::share {

// mc2vr.conf eye_share=off|on (default off — monitor stereo path unchanged).
bool set_enabled(const char *value);

// Called from eye_replay::set_pass at the two pass boundaries with the live
// device + cached backbuffer. `next_pass`: 2 = boundary 1->2 (capture LEFT),
// 0 = boundary 2->0 (capture RIGHT — must run BEFORE the monitor-pin restore
// blits the LEFT snapshot back into the backbuffer).
void on_pass_boundary(uint32_t next_pass, void *device, void *backbuffer);

// Called from eye_replay::on_reset — all surfaces are gone (the device will
// be recreated; the ring re-creates lazily on the next boundary).
void on_reset();

// 10s window report (called from eye::report_window); resets counters.
void report_window();

} // namespace mc2vr::share
