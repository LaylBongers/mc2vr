#include "log.hpp"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace mc2vr {

static HANDLE g_log_file = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION g_log_lock;

void log_init()
{
    // Locate the carrier's own directory via an address inside this module.
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&log_init, &self)) {
        return;
    }

    wchar_t path[MAX_PATH];
    if (GetModuleFileNameW(self, path, MAX_PATH) == 0) {
        return;
    }

    wchar_t *slash = wcsrchr(path, L'\\');
    if (slash) {
        *slash = L'\0';
    }
    wcscat_s(path, L"\\mc2vr_carrier.log");

    // Truncate per launch: one log per run keeps tailing simple; the
    // launcher keeps its own (also truncated) log.
    g_log_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_log_file != INVALID_HANDLE_VALUE) {
        InitializeCriticalSection(&g_log_lock);
    }
}

void describe_code_address(void *addr, char *out, size_t out_size)
{
    HMODULE module = nullptr;
    if (!addr || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     (LPCWSTR)addr, &module)) {
        _snprintf(out, out_size, "unknown:%p", addr);
        out[out_size - 1] = '\0';
        return;
    }

    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(module, path, MAX_PATH);

    const wchar_t *basename = wcsrchr(path, L'\\');
    basename = basename ? basename + 1 : path;

    char name[64] = {};
    WideCharToMultiByte(CP_UTF8, 0, basename, -1, name, sizeof(name) - 1, nullptr, nullptr);

    _snprintf(out, out_size, "%s+0x%08x", name,
              (unsigned)((uintptr_t)addr - (uintptr_t)module));
    out[out_size - 1] = '\0';
}

void log_write(const char *fmt, ...)
{
    if (g_log_file == INVALID_HANDLE_VALUE) {
        return; // not initialized (or file creation failed) — nothing to do
    }

    SYSTEMTIME st;
    GetLocalTime(&st);

    char line[1024];
    int n = snprintf(line, sizeof(line), "[%02u:%02u:%02u.%03u] ",
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    if (n < 0) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    va_end(ap);

    size_t len = strlen(line);
    if (len + 1 >= sizeof(line)) {
        len = sizeof(line) - 2; // truncated; leave room for newline
    }
    line[len] = '\n';
    line[len + 1] = '\0';

    EnterCriticalSection(&g_log_lock);

    DWORD written = 0;
    SetFilePointer(g_log_file, 0, nullptr, FILE_END);
    WriteFile(g_log_file, line, (DWORD)len + 1, &written, nullptr);
    FlushFileBuffers(g_log_file);

    LeaveCriticalSection(&g_log_lock);
}

} // namespace mc2vr
