// debug_wait_census=on: where does the frame time go?
//   - IAT wrappers on the GAME's imports of Sleep / SleepEx /
//     WaitForSingleObject / QueryPerformanceCounter / timeGetTime, counted
//     separately for the render thread and the update (FrameTick) thread:
//     time and calls per frame, top callers by return address. A frame limiter
//     shows up as Sleep time or a QPC spin.
//   - Per-frame phase split: gate | update | pass 1 | pass 2 | tail | outside
//     FrameTick (marks from hooks.cpp and eye_replay.cpp).
// Inert unless enabled. Installed at the first Present (the SecuROM loader has
// filled the IAT by then).
#pragma once

#include <cstdint>

namespace mc2vr::wait_census {

bool set_enabled(const char *value);
void install();

enum class Mark : uint8_t { FrameEntry, GateDone, Pass1, Pass2, PassEnd, FrameExit };
void mark(Mark m);

// 10 s window report (render_dump.cpp poller); resets counters.
void report_window();

} // namespace mc2vr::wait_census
