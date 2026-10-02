// Thread-safe file logger. Writes go to <carrier dir>\mc2vr_carrier.log,
// flushed on every line — the carrier may take the process down with it, so
// buffered logging would lose the interesting tail.
#pragma once

namespace mc2vr {

// Idempotent. Locates the log file next to the carrier DLL itself.
void log_init();

// Formatted line with local timestamp. Safe to call from any thread;
// NULL-safe before log_init (output is dropped) so early failures log.
void log_write(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

} // namespace mc2vr

#define MC2VR_LOG(...) ::mc2vr::log_write(__VA_ARGS__)
