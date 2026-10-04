// OpenXR session wrapper (S4-0): instance, D3D11 session, LOCAL space,
// stereo swapchains, event pump and the frame loop.
#pragma once

#include "d3d.hpp"
#include "pose.hpp"

namespace xrs {

struct Options {
    int maxFrames = 0;  // 0 = run until the session ends
};

// Runs the whole OpenXR lifetime. Returns process exit code.
int run(const Options& opt);

}  // namespace xrs
