// mc2vr launcher: start the game SUSPENDED, inject the carrier DLL, wait for
// the carrier's early hooks to be installed, then resume — hooks are live
// before the game executes anything. Orchestration only — all game memory
// *writes* happen in-process in the carrier.
//
// Layout: launcher + carrier are deployed into <game_dir>\mc2vr\. Paths are
// resolved relative to the launcher's own directory, so the working
// directory doesn't matter:
//   game exe   = <launcher_dir>\..\Mercenaries2.exe
//   carrier    = <launcher_dir>\mc2vr_carrier.dll
//   log file   = <launcher_dir>\mc2vr_launcher.log

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#include "inject.h"

#define GAME_EXE_DEFAULT     L"MERCENARIES2.EXE"
#define HOST_EXE_DEFAULT     L"mc2vr_host.exe"
#define HOST_LOG_DEFAULT     L"mc2vr_host.log"
#define HOST_READY_MARKER    "mc2vr_host: ready"
#define HOST_READY_TIMEOUT_MS (30 * 1000)
#define INJECT_TIMEOUT_MS   (30 * 1000)
#define ACK_TIMEOUT_MS      (15 * 1000)

static wchar_t g_launcher_dir[MAX_PATH];
static wchar_t g_log_path[MAX_PATH];
static FILE *g_log_file;

static void mc2_log(const char *fmt, ...)
{
    va_list ap;

    SYSTEMTIME st;
    GetLocalTime(&st);

    char line[1024];
    int n = snprintf(line, sizeof(line), "[%02u:%02u:%02u.%03u] ",
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    va_end(ap);

    fputs(line, stdout);
    fputc('\n', stdout);
    if (g_log_file) {
        fputs(line, g_log_file);
        fputc('\n', g_log_file);
        fflush(g_log_file);
    }
}

// Strip the final path component. Overwrites in place.
static void path_dirname(wchar_t *path)
{
    wchar_t *slash = wcsrchr(path, L'\\');
    if (slash) {
        *slash = L'\0';
    }
}

static void path_join(wchar_t *out, size_t out_elems, const wchar_t *dir, const wchar_t *name)
{
    _snwprintf(out, out_elems, L"%ls\\%ls", dir, name);
    out[out_elems - 1] = L'\0';
}

// Resolve `rel` (possibly relative) against `base` into a full path.
static BOOL resolve_path(const wchar_t *rel, const wchar_t *base, wchar_t *out, size_t out_elems)
{
    wchar_t tmp[MAX_PATH];
    if (GetFullPathNameW(rel, MAX_PATH, tmp, NULL) == 0) {
        return FALSE;
    }
    (void)base;
    wcscpy_s(out, out_elems, tmp);
    return TRUE;
}

static BOOL file_exists(const wchar_t *path)
{
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

// Scan the log once for a line containing `needle`.
static BOOL log_has_line(const wchar_t *path, const char *needle)
{
    FILE *f = _wfopen(path, L"r");
    if (!f) {
        return FALSE;
    }
    char line[512];
    BOOL found = FALSE;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, needle) != NULL) {
            found = TRUE;
            break;
        }
    }
    fclose(f);
    return found;
}

// Poll the log for a line containing `needle`.
static BOOL wait_for_log_line(const wchar_t *path, const char *needle, DWORD timeout_ms)
{
    for (DWORD waited = 0; waited < timeout_ms; waited += 50) {
        if (log_has_line(path, needle)) {
            return TRUE;
        }
        Sleep(50);
    }
    return log_has_line(path, needle); // final scan at the deadline
}

int main(void) // no arguments: everything is resolved from the install layout
{
    // ---- Locate ourselves ------------------------------------------------
    if (GetModuleFileNameW(NULL, g_launcher_dir, MAX_PATH) == 0) {
        fprintf(stderr, "fatal: cannot determine launcher path\n");
        return 1;
    }
    path_dirname(g_launcher_dir);
    path_join(g_log_path, MAX_PATH, g_launcher_dir, L"mc2vr_launcher.log");

    g_log_file = _wfopen(g_log_path, L"w");
    // Log file is best-effort; console output is the primary channel.

    mc2_log("mc2vr launcher starting (pid=%lu)", GetCurrentProcessId());

    // ---- Resolve paths ----------------------------------------------------
    wchar_t game_dir[MAX_PATH];
    wcscpy_s(game_dir, MAX_PATH, g_launcher_dir);
    path_dirname(game_dir); // launcher lives in <game_dir>\mc2vr

    wchar_t game_exe[MAX_PATH];
    wchar_t carrier_dll[MAX_PATH];
    path_join(game_exe, MAX_PATH, game_dir, GAME_EXE_DEFAULT);
    path_join(carrier_dll, MAX_PATH, g_launcher_dir, L"mc2vr_carrier.dll");

    wchar_t host_exe[MAX_PATH];
    wchar_t host_log[MAX_PATH];
    path_join(host_exe, MAX_PATH, g_launcher_dir, HOST_EXE_DEFAULT);
    path_join(host_log, MAX_PATH, g_launcher_dir, HOST_LOG_DEFAULT);

    if (!file_exists(game_exe)) {
        mc2_log("fatal: game exe not found: %ls", game_exe);
        return 1;
    }
    if (!file_exists(carrier_dll)) {
        mc2_log("fatal: carrier dll not found: %ls", carrier_dll);
        return 1;
    }
    mc2_log("game exe : %ls", game_exe);
    mc2_log("carrier  : %ls", carrier_dll);

    // ---- Start the OpenXR host (S4-1) ----------------------------------------
    // The host owns the VR session and its IPC section; the carrier connects
    // at stage 1 by name. Spawn it first so the section exists before the
    // carrier attaches. Everything here is non-fatal: no host (or a failed
    // host) means the game runs the monitor-stereo path exactly as today.
    char no_host[8];
    if (GetEnvironmentVariableA("MC2VR_NO_HOST", no_host, sizeof no_host) > 0 &&
        no_host[0] != '0') {
        mc2_log("host skipped (MC2VR_NO_HOST set)");
    } else if (!file_exists(host_exe)) {
        mc2_log("host exe not found — running without VR (build with "
                 "cmake -B build/win64 -DCMAKE_TOOLCHAIN_FILE="
                 "cmake/x86_64-w64-mingw32.cmake)");
    } else {
        // Drop the previous run's host log so the ready marker below can only
        // come from this run.
        DeleteFileW(host_log);

        STARTUPINFOW hsi;
        ZeroMemory(&hsi, sizeof(hsi));
        hsi.cb = sizeof(hsi);
        PROCESS_INFORMATION hpi;
        ZeroMemory(&hpi, sizeof(hpi));
        if (!CreateProcessW(host_exe, NULL, NULL, NULL, FALSE,
                            DETACHED_PROCESS, NULL, g_launcher_dir, &hsi, &hpi)) {
            mc2_log("warning: host CreateProcessW failed (%lu) — running "
                    "without VR", GetLastError());
        } else {
            mc2_log("host started (pid=%lu) — waiting for ready", hpi.dwProcessId);
            BOOL ready = FALSE;
            BOOL exitedEarly = FALSE;
            DWORD rc = 0;
            for (DWORD waited = 0; waited < HOST_READY_TIMEOUT_MS; waited += 50) {
                if (log_has_line(host_log, HOST_READY_MARKER)) {
                    ready = TRUE;
                    break;
                }
                if (WaitForSingleObject(hpi.hProcess, 0) == WAIT_OBJECT_0) {
                    // Exited before the marker appeared; re-scan once in case
                    // it was written just before exit.
                    ready = log_has_line(host_log, HOST_READY_MARKER);
                    GetExitCodeProcess(hpi.hProcess, &rc);
                    exitedEarly = TRUE;
                    break;
                }
                Sleep(50);
            }
            if (ready) {
                mc2_log("host ready — proceeding");
            } else if (exitedEarly) {
                mc2_log("warning: host exited early (rc=%lu) — running without "
                        "VR (SteamVR up? see mc2vr_host.log)", rc);
            } else {
                mc2_log("warning: host not ready after %us — running without "
                        "VR", HOST_READY_TIMEOUT_MS / 1000);
            }
            CloseHandle(hpi.hProcess);
            CloseHandle(hpi.hThread);
        }
    }

    // ---- Start the game ----------------------------------------------------
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    // Drop the previous run's carrier log so the markers polled below can
    // only come from this run.
    wchar_t carrier_log[MAX_PATH];
    path_join(carrier_log, MAX_PATH, g_launcher_dir, L"mc2vr_carrier.log");
    DeleteFileW(carrier_log);

    if (!CreateProcessW(game_exe, NULL, NULL, NULL, FALSE, CREATE_SUSPENDED,
                        NULL, game_dir, &si, &pi)) {
        mc2_log("fatal: CreateProcessW failed (%lu)", GetLastError());
        return 1;
    }
    mc2_log("game started suspended (pid=%lu)", pi.dwProcessId);

    // No module-base check here: a suspended process has no module list yet.
    // The carrier verifies base + build lock in-process before hooking.
    DWORD rc;

    // ---- Inject the carrier --------------------------------------------------
    rc = mc2_inject_dll(pi.hProcess, carrier_dll, INJECT_TIMEOUT_MS);
    if (rc != 0) {
        mc2_log("fatal: carrier injection failed (rc=%lu)", rc);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        return 1;
    }
    mc2_log("carrier injected");

    // ---- Wait for the carrier's early hooks, then resume ---------------------
    // The marker is logged at the end of carrier stage 1 (also when the
    // build lock fails and the carrier stays idle). Poll for the actual line,
    // not bare file existence — the file exists before its first line.
    BOOL acked = wait_for_log_line(carrier_log, "early init done", ACK_TIMEOUT_MS);
    if (!acked) {
        mc2_log("fatal: carrier never reported early init — not resuming the game");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 1;
    }
    if (ResumeThread(pi.hThread) == (DWORD)-1) {
        mc2_log("fatal: ResumeThread failed (%lu)", GetLastError());
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 1;
    }
    mc2_log("early hooks in place — game resumed");
    CloseHandle(pi.hThread);
    // Game keeps running on its own from here.
    CloseHandle(pi.hProcess);

    // Let late-stage init finish a little so the dump below is informative.
    wait_for_log_line(carrier_log, "init complete", 10000);

    FILE *f = _wfopen(carrier_log, L"r");
    if (f) {
        // Rewind and dump everything written so far.
        fseek(f, 0, SEEK_SET);
        mc2_log("---- carrier log ----");
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            fputs(line, stdout);
            if (g_log_file) {
                fputs(line, g_log_file);
            }
        }
        fclose(f);
        mc2_log("---- end carrier log ----");
    } else {
        mc2_log("warning: carrier ack not found after %us — check mc2vr_carrier.log",
                ACK_TIMEOUT_MS / 1000);
    }

    mc2_log("done — game is running with the carrier attached");
    if (g_log_file) {
        fclose(g_log_file);
    }
    return 0;
}
