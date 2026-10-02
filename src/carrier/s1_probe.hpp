// S2 channel module: the GPU-boundary per-eye injection mechanism.
//
// S1 (draw-camera hunt) is COMPLETE (2026-10-02, 17 runs — evidence and
// per-run history in docs/s1_camera_hunt.md). All S1 probing code has been
// REMOVED: patch windows A–O, consumer brackets, pose captures, matrix
// classification, exfil, vsclock/vspose controls, telemetry taps. What
// remains is only what S2 builds on:
//   - on_set_vs_constant: the SetVertexShaderConstantF tap (device VmtHook
//     slot 94 — the draw-consumption point). Maintains the VS register
//     cache (the S2a camera-row identification input) and applies the
//     ambient GPU-boundary rewrite (verified visible, S1 runs 16–17).
//   - set_ambient_rewrite: the mc2vr.conf toggle (gpu_boundary_rewrite=
//     off|on|pulse) — the persistent verification mode for the rewrite.
//   - report_window: slim 10s telemetry (upload volume, rewrite volume).
#pragma once

#include <cstdint>

namespace mc2vr::s1 {

// No MidHooks remain in this module (the S1 hunt sites are gone). Kept as
// a log point so init.cpp is unchanged; the load-bearing channel is the
// device VmtHook installed by device.cpp.
void install();

// Tap from device.cpp's SetVertexShaderConstantF handler. Runs on the main
// thread, before the driver call — buffer modifications are what the draw
// consumes.
void on_set_vs_constant(uint32_t start_register, const float *data,
                        uint32_t vec4_count);

// Ambient rewrite mode (mc2vr.conf gpu_boundary_rewrite=off|on|pulse).
// off restores pass-through. Returns false on unrecognized values.
bool set_ambient_rewrite(const char *value);

// 10s window report (called from render_dump.cpp's poller); resets the
// window aggregates.
void report_window();

} // namespace mc2vr::s1
