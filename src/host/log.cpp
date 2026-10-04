#include "log.hpp"

#include <windows.h>

#include <cstdarg>
#include <cstdio>

namespace {
FILE* g_file = nullptr;
}

namespace hostlog {

void open(const wchar_t* path) {
    g_file = _wfopen(path, L"w");
}

void write(const char* fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[1100];
    snprintf(line, sizeof line, "[%02d:%02d:%02d.%03d] %s\n", t.wHour, t.wMinute, t.wSecond,
             t.wMilliseconds, msg);
    fputs(line, stdout);
    fflush(stdout);
    if (g_file) {
        fputs(line, g_file);
        fflush(g_file);
    }
}

}  // namespace hostlog
