// One-shot LtiRenderer_BeginSubmit entry probe (MidHook): logs `this`, its
// vtable and the caller for the first 3 calls — base-vs-derived vtable and
// the frame driver (docs/reverse_engineering/frame_chain.md). Instrument
// only; zero behavior change.
#pragma once

namespace mc2vr::beginsubmit_probe {

// Attach-time (game suspended). Failure is logged and non-fatal.
void install();

} // namespace mc2vr::beginsubmit_probe
