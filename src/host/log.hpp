// Host log: mc2vr_host.log beside the exe (stdout is mirrored so the
// launcher/selftest can wait for the "ready" line).
#pragma once

namespace hostlog {
void open(const wchar_t* path);
void write(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
}  // namespace hostlog
