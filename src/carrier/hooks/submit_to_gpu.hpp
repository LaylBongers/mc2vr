// PgPrimitive_SubmitToGPU (0x00855690): the two-pass stereo replay. An inline
// hook runs the original twice per frame, LEFT then RIGHT
// (vr/eye_replay.cpp run_frame). Always on; without it there is no RIGHT eye.
#pragma once

namespace mc2vr::hooks {

void install_submit_to_gpu();

} // namespace mc2vr::hooks
