// The crash logger (src/carrier/crash_log.cpp) runs only when the game dies,
// so it gets one end-to-end check here, under Wine like the game: the test
// re-runs itself as a child that installs the logger and writes through a bad
// pointer, then reads the log the child left next to the exe
// (mc2vr_carrier.log) and checks the report's lines.
//
//   cmake --build build/win32 --target mc2vr_crash_log_test && wine build/win32/bin/mc2vr_crash_log_test.exe

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "crash_log.hpp"
#include "log.hpp"

// The logger reads these from hooks/frame_tick; the test has no game.
namespace mc2vr::hooks {
uint64_t frame_count() { return 1234; }
uint32_t update_thread_id() { return GetCurrentThreadId(); }
} // namespace mc2vr::hooks

namespace {

__attribute__((noinline)) void crash_here(volatile int *p)
{
    *p = 0x42;  // write to an unmapped address
}

int child()
{
    mc2vr::log_init();
    mc2vr::crash_log::install();
    crash_here((volatile int *)(uintptr_t)0x10);
    return 0;  // not reached
}

std::wstring self_dir()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (wchar_t *slash = wcsrchr(path, L'\\')) *slash = L'\0';
    return path;
}

bool has(const std::string &log, const char *needle)
{
    const bool ok = log.find(needle) != std::string::npos;
    printf("  %-48s %s\n", needle, ok ? "ok" : "MISSING");
    return ok;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "child") == 0) {
        return child();
    }

    const std::wstring log_path = self_dir() + L"\\mc2vr_carrier.log";
    DeleteFileW(log_path.c_str());

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" child";
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        printf("FAIL: CreateProcess %lu\n", GetLastError());
        return 1;
    }
    const DWORD t0 = GetTickCount();
    WaitForSingleObject(pi.hProcess, 30000);
    const DWORD child_ms = GetTickCount() - t0;
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    printf("child exit code %08lX\n", code);

    std::string log;
    FILE *f = _wfopen(log_path.c_str(), L"rb");
    if (f == nullptr) {
        printf("FAIL: no log written\n");
        return 1;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) log.append(buf, n);
    fclose(f);

    bool ok = code == EXCEPTION_ACCESS_VIOLATION;
    printf("  %-48s %s\n", "child died with an access violation", ok ? "ok" : "NO");
    // With SEM_NOGPFAULTERRORBOX the child ends right after the report; Wine's
    // crash dialog used to hold it ~5 s.
    printf("  %-48s %s (%lu ms)\n", "no crash dialog (child ended quickly)", child_ms < 3000 ? "ok" : "NO", child_ms);
    ok &= child_ms < 3000;
    ok &= has(log, "crash: unhandled-exception logger installed");
    ok &= has(log, "CRASH: unhandled access violation (code C0000005");
    ok &= has(log, "| frame 1234");
    ok &= has(log, "CRASH: write of address 00000010");
    ok &= has(log, "CRASH: EAX=");
    ok &= has(log, "CRASH: code at EIP:");
    ok &= has(log, "CRASH: stack:");
    ok &= has(log, "CRASH: end of report");
    size_t reports = 0;
    for (size_t at = log.find("CRASH: end of report"); at != std::string::npos; at = log.find("CRASH: end of report", at + 1)) {
        reports++;
    }
    printf("  %-48s %s (%zu)\n", "exactly one report", reports == 1 ? "ok" : "NO", reports);
    ok &= reports == 1;
    printf(ok ? "PASS\n" : "FAIL\n");
    if (!ok) printf("--- log ---\n%s\n", log.c_str());
    return ok ? 0 : 1;
}
