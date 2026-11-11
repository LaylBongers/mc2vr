// Crash logger — see crash_log.hpp.

#include "crash_log.hpp"

#include <windows.h>

#include <cstdint>
#include <cstdio>

#include "hooks/frame_tick.hpp"
#include "log.hpp"

namespace mc2vr::crash_log {

namespace {

LPTOP_LEVEL_EXCEPTION_FILTER g_prev = nullptr;  // the filter we chain to
volatile LONG g_in_filter = 0;

bool readable(uintptr_t addr, size_t len)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((const void *)addr, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT ||
        (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return false;
    }
    return addr + len <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

bool executable(uintptr_t addr)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((const void *)addr, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT) {
        return false;
    }
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

const char *code_name(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_BREAKPOINT: return "breakpoint";
    case EXCEPTION_SINGLE_STEP: return "single step";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case 0xE06D7363: return "C++ exception";
    default: return "exception";
    }
}

void log_report(EXCEPTION_POINTERS *ep)
{
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    const CONTEXT *c = ep->ContextRecord;
    const uint32_t tid = GetCurrentThreadId();
    char where[96];
    describe_code_address(er->ExceptionAddress, where, sizeof(where));
    log_write_crash("CRASH: unhandled %s (code %08lX, flags %lX) at %s (VA %p) | thread %u%s | frame %llu",
                    code_name(er->ExceptionCode), er->ExceptionCode, er->ExceptionFlags, where, er->ExceptionAddress,
                    tid, tid == hooks::update_thread_id() ? " (update thread)" : "",
                    (unsigned long long)hooks::frame_count());
    if ((er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || er->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        er->NumberParameters >= 2) {
        const ULONG_PTR op = er->ExceptionInformation[0];
        log_write_crash("CRASH: %s of address %08lX", op == 0 ? "read" : op == 1 ? "write" : op == 8 ? "execute" : "access",
                        (unsigned long)er->ExceptionInformation[1]);
    }
    log_write_crash("CRASH: EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX ESI=%08lX EDI=%08lX EBP=%08lX ESP=%08lX "
                    "EIP=%08lX EFL=%08lX",
                    c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp, c->Eip, c->EFlags);

    // Code bytes at EIP (for matching against Ghidra / host disassembly).
    if (readable(c->Eip, 16)) {
        const uint8_t *b = (const uint8_t *)(uintptr_t)c->Eip;
        log_write_crash("CRASH: code at EIP: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X "
                        "%02X %02X",
                        b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14],
                        b[15]);
    }

    // Stack scan: values that point into executable memory (candidate return
    // addresses; with FPO there is no reliable frame chain). Up to 24 hits in
    // the first 1 KB above ESP.
    char line[700];
    int k = 0, hits = 0;
    for (uint32_t off = 0; off < 1024 && hits < 24; off += 4) {
        const uintptr_t slot = c->Esp + off;
        if (!readable(slot, 4)) break;
        const uintptr_t v = *(const uint32_t *)slot;
        if (v < 0x10000 || !executable(v)) continue;
        char d[96];
        describe_code_address((void *)v, d, sizeof(d));
        k += snprintf(line + k, sizeof(line) - k, " [esp+%X]%s", off, d);
        hits++;
        if (k > (int)sizeof(line) - 120) {
            log_write_crash("CRASH: stack:%s", line);
            k = 0;
        }
    }
    if (k > 0) log_write_crash("CRASH: stack:%s", line);
    log_write_crash("CRASH: end of report (chaining to %s)", g_prev ? "the previous filter" : "the default handler");
}

LONG WINAPI filter(EXCEPTION_POINTERS *ep)
{
    if (InterlockedExchange(&g_in_filter, 1) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;  // reentered (a chained filter called us again, or a nested fault)
    }
    // Once per distinct exception: Wine calls the filter again after its
    // default handler (2026-11-11, tools/test/test_crash_log.cpp).
    static void *s_last_addr = nullptr;
    static DWORD s_last_code = 0;
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionAddress != s_last_addr || er->ExceptionCode != s_last_code) {
        s_last_addr = er->ExceptionAddress;
        s_last_code = er->ExceptionCode;
        log_report(ep);
    }
    const LONG r = g_prev ? g_prev(ep) : EXCEPTION_CONTINUE_SEARCH;
    InterlockedExchange(&g_in_filter, 0);
    return r;
}

} // namespace

void install()
{
    g_prev = SetUnhandledExceptionFilter(filter);
    // No crash dialog: Wine's "encountered a serious problem" box opens behind
    // the fullscreen game and the process waits on it — a crash looked like a
    // freeze (2026-11-11). With it off, the process ends after the report.
    SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX);
    MC2VR_LOG("crash: unhandled-exception logger installed (previous filter %p), crash dialog off", (void *)g_prev);
}

namespace {

// Hang watchdog state (poller thread only).
uint64_t g_wd_frame = 0;
DWORD g_wd_since = 0;   // tick of the last frame change
bool g_wd_reported = false;
constexpr DWORD kHangMs = 20000;

void report_hang(DWORD stalled_ms)
{
    const uint32_t tid = hooks::update_thread_id();
    HANDLE h = tid ? OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, tid) : nullptr;
    if (h == nullptr) {
        MC2VR_LOG("HANG: no frame for %lu s; update thread %u not inspectable", stalled_ms / 1000, tid);
        return;
    }
    // Capture only while suspended — no loader-lock or heap calls in that
    // window (the stuck thread may hold either); format after resuming.
    CONTEXT c = {};
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    static uint32_t stack[256];
    uint32_t nstack = 0;
    bool got = false;
    if (SuspendThread(h) != (DWORD)-1) {
        got = GetThreadContext(h, &c) != 0;
        if (got) {
            for (; nstack < 256 && readable(c.Esp + nstack * 4, 4); nstack++) {
                stack[nstack] = *(const uint32_t *)(uintptr_t)(c.Esp + nstack * 4);
            }
        }
        ResumeThread(h);
    }
    CloseHandle(h);
    if (!got) {
        MC2VR_LOG("HANG: no frame for %lu s; update thread %u context unavailable", stalled_ms / 1000, tid);
        return;
    }
    char where[96];
    describe_code_address((void *)(uintptr_t)c.Eip, where, sizeof(where));
    MC2VR_LOG("HANG: no frame for %lu s (frame %llu); update thread %u at %s (EIP %08lX) | EAX=%08lX EBX=%08lX "
              "ECX=%08lX EDX=%08lX ESI=%08lX EDI=%08lX EBP=%08lX ESP=%08lX",
              stalled_ms / 1000, (unsigned long long)g_wd_frame, tid, where, c.Eip, c.Eax, c.Ebx, c.Ecx, c.Edx,
              c.Esi, c.Edi, c.Ebp, c.Esp);
    char line[700];
    int k = 0, hits = 0;
    for (uint32_t i = 0; i < nstack && hits < 24; i++) {
        const uintptr_t v = stack[i];
        if (v < 0x10000 || !executable(v)) continue;
        char d[96];
        describe_code_address((void *)v, d, sizeof(d));
        k += snprintf(line + k, sizeof(line) - k, " [esp+%X]%s", i * 4, d);
        hits++;
        if (k > (int)sizeof(line) - 120) {
            MC2VR_LOG("HANG: stack:%s", line);
            k = 0;
        }
    }
    if (k > 0) MC2VR_LOG("HANG: stack:%s", line);
}

} // namespace

void watchdog_tick()
{
    const uint64_t f = hooks::frame_count();
    const DWORD now = GetTickCount();
    if (f != g_wd_frame || g_wd_since == 0) {
        if (g_wd_reported) {
            MC2VR_LOG("HANG: frames resumed after %lu s (frame %llu)", (now - g_wd_since) / 1000,
                      (unsigned long long)f);
        }
        g_wd_frame = f;
        g_wd_since = now;
        g_wd_reported = false;
        return;
    }
    if (!g_wd_reported && now - g_wd_since >= kHangMs) {
        g_wd_reported = true;
        report_hang(now - g_wd_since);
    }
}

void reassert()
{
    const LPTOP_LEVEL_EXCEPTION_FILTER current = SetUnhandledExceptionFilter(filter);
    if (current != filter) {
        g_prev = current;
        MC2VR_LOG("crash: filter had been replaced by %p — logger back on top, chaining to it", (void *)current);
    }
}

} // namespace mc2vr::crash_log
