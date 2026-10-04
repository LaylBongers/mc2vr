// S4-2: per-eye LDR capture + SteamVR compositor submit (docs/s4_handover.md
// §S4 engineering list item 2).
//
// The game's own final composite already produces the tonemapped LDR per-eye
// images on the BACKBUFFER at the pass boundaries (counter-proven, run 3/6b):
//   - 1->2 boundary: backbuffer holds pass 1's final LEFT composite
//   - 2->0 boundary: backbuffer holds pass 2's final RIGHT composite,
//     BEFORE the monitor pin's restore overwrites it (the caller blits this
//     module's captures in ahead of the pin restore)
// This module snapshots those two instants into carrier-owned D3D9 textures
// (DEFAULT pool, RENDERTARGET usage — StretchRect needs the dst to be an RT)
// and submits the pair via IVRCompositor::Submit / PostPresentHandoff.
//
// Threading: everything runs on the render thread (called from eye::set_pass,
// inside the SubmitToGPU hook) — the thread all D3D device use happens on, and
// the thread OpenVR's Submit contract requires.
//
// The DXVK boot-time interop question (does Submit with a D3D9 texture work
// with the interop disarmed?) is deliberately answered by the FIRST LIVE RUN,
// not by more RE: Submit is now a well-formed call on a verified session (the
// run-5 hang was the self-arg ABI bug, fixed; the known-poison init_registry
// path is gated off), it returns EVRCompositorError in the ordinary failure
// mode, and every distinct error code is logged so the run picks the next
// design step (error -> decision table in docs/s4_handover.md §S4 item 2).
//
// mc2vr.conf openvr_submit=off|on (default off). Requires openvr=on.
#pragma once

#include <cstdint>

namespace mc2vr::submit {

// mc2vr.conf openvr_submit=off|on (default off).
bool set_enabled(const char *value);

// Pass-boundary hook, called from eye::set_pass (render thread) on real pass
// transitions only. `backbuffer` is eye_replay's cached swapchain backbuffer
// (may be null early in boot — the call is then a no-op).
//   prev=1 next=2: capture the LEFT eye (pass-1 composite)
//   prev=2 next=0: capture the RIGHT eye, then Submit(L), Submit(R)
//                  (PostPresentHandoff REMOVED after run 7 — live-proven
//                  crash suspect; see hmd_submit.cpp)
void on_boundary(void *device, uint32_t prev_pass, uint32_t next_pass,
                 void *backbuffer);

// Called from eye::on_reset BEFORE the original Reset — release the capture
// textures while they still exist; re-created lazily after Reset.
void on_reset();

// 10s window report (render_dump.cpp poller); resets counters.
void report_window();

} // namespace mc2vr::submit
