// Camera-pose injection probe tooling (2026-10-06 session: experiments E2/E2b,
// verdicts recorded in docs/stereo_improvements_plan.md § Experiment log).
//
// Two retained instruments, both default OFF:
//
// 1. debug_entry_inject=on — the ViewEntry entry-injection probe (E2, verdict:
//    NEGATIVE — the entry pos/quat are output channels of the staged round-trip
//    and are reverted within one frame; kept for re-tests and as the template
//    for future injection probes). Injects a lateral position oscillation
//    (+A*right*sin(2*pi*f*t)) + serial bumps (frustrum_cull_plan.md D2 protocol)
//    into every live type-2 view whose pos7c4 is within 100 units of the
//    decomposed RAW game camera (view::get_game_camera — requires
//    view_row_rewrite=hmd|hmd_identity AND the HMD tracked). The per-window
//    SLOPE is the phase-immune verdict metric (≈1 = the draw camera follows,
//    ≈0 = it does not) — the reusable injection-verification instrument.
//      debug_entry_inject_offset=<world units>   amplitude (default 8.0)
//      debug_entry_inject_hz=<float>             frequency (default 0.25)
//
// 2. debug_cambuilder_dump=on — the draw-camera VP builder entry hook (E2b):
//    MidHook at ViewContext_BuildCameraConstants (0x008591ac) logging the
//    per-call camera object ([arg+0x28], self-indexed 0x70-stride array —
//    stack-local in gameplay) and dumping the active entry once per distinct
//    address. Address/layout discovery instrument (entry layout constants:
//    MC2_VCCAM_ENTRY_* in game_addresses.h).
#pragma once

#include <cstdint>

#include <safetyhook.hpp>

namespace mc2vr::injectprobe {

// debug_entry_inject=on|off — the ViewEntry injection probe (E2, negative).
bool set_entry_inject_enabled(const char *value);
void set_entry_inject_offset(float units);  // debug_entry_inject_offset
void set_entry_inject_hz(float hz);         // debug_entry_inject_hz

// debug_cambuilder_dump=on|off — builder-entry camera-object dump (E2b).
bool set_cambuilder_enabled(const char *value);

// Install the builder-entry MidHook. Call once from render_dump::install_early();
// no-op when debug_cambuilder_dump is off.
void install();

// Per-view callback from render_dump's view-loop MidHook (main thread, once
// per active view per frame, before the staging). No-op when the entry
// injection probe is disabled.
void on_view(uint32_t idx, uint32_t type, const uint8_t *entry);

// 10s window report (render_dump poller thread).
void report_window();

}  // namespace mc2vr::injectprobe
