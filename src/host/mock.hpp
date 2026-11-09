// --mock: no OpenXR runtime. Synthetic pose + test-pattern eye textures so
// IPC and the game run without an HMD.
#pragma once

namespace mock {

// Runs `frames` frames (0 = until killed) at ~90 Hz.
int run(int frames);

}  // namespace mock
