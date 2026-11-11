// Crash logger (always on; docs/debugging.md "Crash reports"): an unhandled-
// exception filter that writes what crashed to the carrier log before the
// process dies — exception code and address (module+offset), access details,
// registers, the code bytes at EIP, the thread (update thread or not), the
// frame, and a scan of the stack for return-address-like values. Then it
// chains to the filter it replaced.
//
// Only exceptions nobody handled reach it: SecuROM's and the game's own
// first-chance exceptions never do. The game's CRT may install its own filter
// after ours; reassert() (10 s poller) puts ours back on top and keeps the
// replacement as the one we chain to. A reentrancy guard stops a filter that
// chains back to ours from looping.
#pragma once

namespace mc2vr::crash_log {

// Attach time, before the game's first instruction.
void install();

// Re-installs the filter if something replaced it (10 s poller). Logs once per
// replacement.
void reassert();

// Hang watchdog (250 ms poller tick): when the frame counter hasn't moved for
// 20 s, logs a HANG: report — the update thread's registers and a stack scan,
// captured while it is briefly suspended — and later when frames resume. A
// long synchronous load can trip it; the report then shows the loader.
void watchdog_tick();

} // namespace mc2vr::crash_log
