// S1: consumer + field-use runtime instrumentation (docs/stereo_design.md
// §S1 + §S1 run results). Two revisions:
//   S1  (first run): consumer bracket + SetTransform/SetViewport logging +
//        m[1] residency patch + satellite classification. ANSWERS: consumer
//        runs at pipeline time; element = {size,ptr} descriptors confirmed;
//        SetTransform NEVER called (shader-driven engine) -> camera evidence
//        must come from SetVertexShaderConstantF; satellite frames confirmed.
//   S1b (second run): SetVertexShaderConstantF (slot 94) matrix reconstruction
//        + classification; residency patch v2 on a LIVE head view (nonzero m[0]
//        translation) with two spaced windows — window A patches m[1][3],
//        window B patches the camera staging slot pos (frameCtx+0xc2110+
//        head*0x30) — the visual nudge (or the VS-constant match) then
//        discriminates the draw-camera channel; counter semantics fixed
//        (+0x10 = 32-bit cumulative, monotonic); element dumps re-armed to
//        fire only when the frame has world views; satellite detection uses
//        the uncapped per-frame view total.
// New MidHooks live here; the M3 handlers (render_dump.cpp) and the device
// VmtHook (device.cpp) feed this module via the note_*/on_* taps, so the M3
// ambient telemetry is extended, not replaced. Handlers run on the main
// thread (single-threaded render path); report_window() runs on the queue
// poller thread like the M3 window reports — same loose-read policy.
#pragma once

#include <cstdint>

namespace mc2vr::s1 {

// Installs the S1 MidHooks (pre-VM 0x004c99f9 + RenderFrame entry 0x00855690).
// Best-effort: failures are logged and don't stop the rest.
void install();

// Tap from render_dump.cpp's view-loop MidHook (0x0048e9ea): one call per
// walked view. `flags` = the full ViewRef+0x14 dword (low16 = type);
// `frame_ctx` = EBX at that site = the frame-ctx object (0x680 block at
// +0xd2950, camera staging at +0xc2110), validated structurally before use.
void note_view(uint32_t idx, uint32_t type, uint32_t flags, uintptr_t frame_ctx);

// Tap from render_dump.cpp's opcode MidHook (0x008569f5): first stream
// command of each frame records countersA (bracket point 3).
void note_stream_opcode();

// Tap from render_dump.cpp's slot-4 handler (EndOfFrameHook): bracket point 4.
void note_end_of_frame();

// Taps from device.cpp's VmtHook handlers. SetTransform is kept for
// completeness (S1 proved it unused, but the count documents that per run).
void on_set_transform(uint32_t state, const float *m);
void on_set_viewport(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                     float minz, float maxz);
void on_set_vs_constant(uint32_t start_register, const float *data, uint32_t vec4_count);

// 10s window report (called from render_dump.cpp's poller); resets the
// window aggregates. Process-lifetime one-shots are NOT reset.
void report_window();

} // namespace mc2vr::s1
