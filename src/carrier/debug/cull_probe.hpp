// debug_cull_probe=on: which frustum plane culls what, by camera pitch
// (docs/culling.md "OPEN: most scenery culled when looking steeply up or down").
//
// Wraps Frustum_TestPoints (0x00857e70, stdcall (frustum, points, count,
// mask*) -> AL), the engine's plane test: every rejection is attributed to
// the caller (return address), the frustum (its offset inside a render-shell
// view block, e.g. ctx+0x38 = the view, or "other" for stack copies) and the
// plane that rejected (0 left, 1 bottom, 2 right, 3 top, 4 near, 5 far — the
// Frustum_Build order), binned by the main camera's pitch (10° bins, from
// view_rewrite). Per 10 s: tests and rejects per caller and pitch bin, and
// the rejects per (caller, frustum, plane). Log-only.
#pragma once

namespace mc2vr::cull_probe {

// mc2vr.conf debug_cull_probe=on|off (default off). False on bad input.
bool set_enabled(const char *value);

void install();

// 10 s window report; resets counters. No-op when off.
void report_window();

} // namespace mc2vr::cull_probe
