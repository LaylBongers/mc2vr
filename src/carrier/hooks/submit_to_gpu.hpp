// PgPrimitive_SubmitToGPU (0x00855690): the two-pass stereo replay. An inline
// hook on its entry runs one frame's submission twice, LEFT then RIGHT
// (vr/eye_replay.cpp run_frame). Always on; without it there is no RIGHT eye.
// Each pass runs our reimplementation (reimpl/render/submit_to_gpu.cpp), or
// the original with debug_reimpl=off; debug_reimpl=shadow:PgPrimitive_SubmitToGPU sets its own pair.
#pragma once

namespace mc2vr::hooks {

void install_submit_to_gpu();

// The original function, through the hook's trampoline.
void call_submit_to_gpu_original();

// Replaces the per-pass functions (debug/shadow; attach time, before the
// first frame).
void set_submit_passes(void (*pass1)(), void (*pass2)());

} // namespace mc2vr::hooks
