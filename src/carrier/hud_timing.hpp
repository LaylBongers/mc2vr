// S4-5 HUD/2D — RESOLVED (2026-10-06, live-verified): both render queues are
// consumed once per frame, entirely between SubmitToGPU entry and BeginSubmit's
// Present (before pass 1 draws); both passes then walk the same record table,
// so 2D/HUD content lands in both eyes' composites. One-eye HUD is impossible;
// no host quad layer is needed. Full evidence and protocol derivation:
// docs/reverse_engineering/render_path.md (queue counter open item).
//
// What remains: a one-shot raw-counter diagnostic burst (first 3 frames after
// the first 10s window) for future queue-protocol questions. Read-only.
#pragma once

#include <cstdint>

namespace mc2vr::hud_timing {

// From device::present_hook — every Present (pass-1 and pass-2 BeginSubmit
// with the second draw pass; 1/frame in mono mode).
void on_present();

// From eye_replay::set_pass — `pass` is the NEW pass value at a boundary transition
// (1 = pass-1 start, 2 = pass 1 done, 0 = pass 2 done).
void on_boundary(uint32_t pass);

}  // namespace mc2vr::hud_timing
