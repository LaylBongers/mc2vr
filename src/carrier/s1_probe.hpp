// S1: consumer + field-use runtime instrumentation (docs/stereo_design.md
// §S1). One run answers the four open semantics the SecuROM wall hid:
//   S1.1  consumer bracket — MidHook at the pipeline call site of the VM'd
//         packet interpreter (0x004c99f9, the call itself) + MidHook at
//         RenderShell_RenderFrame entry (0x00855690): does countersA.low
//         (consumer half) advance between the two = interpreter confirmed
//         at pipeline time. Ring elements are hex-dumped once at the
//         candidate positions to verify the {0x30,0x810,0x680} pair layout.
//   S1.2  camera attribution — device SetTransform/SetViewport VmtHook slots
//         (44/47, installed in device.cpp): classify each D3DTS_VIEW /
//         D3DTS_PROJECTION matrix against the live ViewEntry matrices, the
//         frame-ctx 0x680 block, and the two live primary subobjects.
//   S1.3  residency proof — patch the head view's m[1] translation at the
//         pre-VM hook for a few frames (restore each frame); if that frame's
//         SetTransform shows the patched value, the consumer derefs the LIVE
//         entry at consume time (clone-at-stage S3 proceeds as designed).
//   S1.4  satellite classification — per-frame (idx, type, flags) view-list
//         logging (signature-deduped) to pick the views S3 duplicates.
// New MidHooks live here; the existing M3 handlers (render_dump.cpp) and the
// device VmtHook (device.cpp) feed this module via the note_*/on_* taps, so
// the M3 ambient telemetry is extended, not replaced. Handlers run on the
// main thread (single-threaded render path); report_window() runs on the
// queue poller thread like the M3 window reports — same loose-read policy.
#pragma once

#include <cstdint>

namespace mc2vr::s1 {

// Installs the two S1 MidHooks (best-effort: failures are logged and don't
// stop the rest).
void install();

// Tap from render_dump.cpp's view-loop MidHook (0x0048e9ea): one call per
// walked view. `flags` = the full ViewRef+0x14 dword (low16 = type);
// `frame_ctx` = EBX at that site = the frame-ctx object (0x680 block at
// +0xd2950), validated structurally before any use.
void note_view(uint32_t idx, uint32_t type, uint32_t flags, uintptr_t frame_ctx);

// Tap from render_dump.cpp's opcode MidHook (0x008569f5): first stream
// command of each frame records countersA.low (bracket point 3).
void note_stream_opcode();

// Tap from render_dump.cpp's slot-4 handler (EndOfFrameHook): bracket point 4.
void note_end_of_frame();

// Taps from device.cpp's SetTransform/SetViewport VmtHook handlers.
void on_set_transform(uint32_t state, const float *m);
void on_set_viewport(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                     float minz, float maxz);

// 10s window report (called from render_dump.cpp's poller); resets the
// window aggregates. Process-lifetime one-shots are NOT reset.
void report_window();

} // namespace mc2vr::s1
