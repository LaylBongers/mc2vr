#include "wait_census.hpp"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "log.hpp"

namespace mc2vr::wait_census {

namespace {

// Game IAT slots (.rdata; the game's code calls through these).
constexpr uintptr_t IAT_SLEEP = 0x00b0518c;
constexpr uintptr_t IAT_SLEEPEX = 0x00b0519c;
constexpr uintptr_t IAT_WFSO = 0x00b05090;
constexpr uintptr_t IAT_QPC = 0x00b05124;
constexpr uintptr_t IAT_TIMEGETTIME = 0x00b05520;

bool g_enabled = false;
bool g_installed = false;
DWORD g_main_tid = 0;
double g_qpc_to_ms = 0.0;

using Sleep_t = VOID(WINAPI *)(DWORD);
using SleepEx_t = DWORD(WINAPI *)(DWORD, BOOL);
using Wfso_t = DWORD(WINAPI *)(HANDLE, DWORD);
using Qpc_t = BOOL(WINAPI *)(LARGE_INTEGER *);
using Tgt_t = DWORD(WINAPI *)(void);
Sleep_t o_sleep = nullptr;
SleepEx_t o_sleepex = nullptr;
Wfso_t o_wfso = nullptr;
Qpc_t o_qpc = nullptr;
Tgt_t o_tgt = nullptr;

inline double now_ms()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);  // the carrier's own import, not the game slot
    return (double)t.QuadPart * g_qpc_to_ms;
}

// ---- caller attribution -----------------------------------------------------
struct Caller {
    uintptr_t ret;
    uint32_t calls;
    double ms;
    uint32_t arg_max;  // largest timeout argument seen (Sleep/SleepEx/WFSO)
    double phase_ms[6];  // time by frame phase (see kPhaseNames)
};
// Frame phase the main thread is in (set by mark()); 0 gate .. 5 outside.
int g_cur_phase = 5;
const char *const kPhaseNames[6] = {"gate", "update", "pass1", "pass2", "tail", "outside"};
constexpr int kCallers = 16;
struct Census {
    uint64_t calls = 0;
    double ms = 0.0;
    Caller top[kCallers] = {};
    void add(uintptr_t ret, double dt, uint32_t arg)
    {
        calls++;
        ms += dt;
        for (Caller &c : top) {
            if (c.ret == ret || c.ret == 0) {
                c.ret = ret;
                c.calls++;
                c.ms += dt;
                c.phase_ms[g_cur_phase] += dt;
                if (arg > c.arg_max) c.arg_max = arg;
                return;
            }
        }
    }
    void reset() { *this = Census(); }
};
// [0] = render thread (first Present), [1] = update thread (FrameTick).
Census g_sleep[2], g_sleepex[2], g_wfso[2], g_qpc[2], g_tgt[2];
volatile DWORD g_update_tid = 0;  // set at the FrameEntry mark

// Which tracked thread is calling: 0 render, 1 update, -1 other.
int which()
{
    const DWORD t = GetCurrentThreadId();
    if (t == g_main_tid) return 0;
    if (t == g_update_tid) return 1;
    return -1;
}

VOID WINAPI w_sleep(DWORD ms)
{
    const int w = which();
    if (w < 0) return o_sleep(ms);
    const uintptr_t ret = (uintptr_t)__builtin_return_address(0);
    const double t0 = now_ms();
    o_sleep(ms);
    g_sleep[w].add(ret, now_ms() - t0, ms);
}
DWORD WINAPI w_sleepex(DWORD ms, BOOL alertable)
{
    const int w = which();
    if (w < 0) return o_sleepex(ms, alertable);
    const uintptr_t ret = (uintptr_t)__builtin_return_address(0);
    const double t0 = now_ms();
    const DWORD r = o_sleepex(ms, alertable);
    g_sleepex[w].add(ret, now_ms() - t0, ms);
    return r;
}
DWORD WINAPI w_wfso(HANDLE h, DWORD ms)
{
    const int w = which();
    if (w < 0) return o_wfso(h, ms);
    const uintptr_t ret = (uintptr_t)__builtin_return_address(0);
    const double t0 = now_ms();
    const DWORD r = o_wfso(h, ms);
    g_wfso[w].add(ret, now_ms() - t0, ms);
    return r;
}
BOOL WINAPI w_qpc(LARGE_INTEGER *out)
{
    const int w = which();
    if (w >= 0) g_qpc[w].add((uintptr_t)__builtin_return_address(0), 0.0, 0);
    return o_qpc(out);
}
DWORD WINAPI w_tgt()
{
    const int w = which();
    if (w >= 0) g_tgt[w].add((uintptr_t)__builtin_return_address(0), 0.0, 0);
    return o_tgt();
}

template <typename T>
bool patch(uintptr_t slot, T wrapper, T *orig, const char *name)
{
    void **p = (void **)slot;
    DWORD old;
    if (!VirtualProtect(p, sizeof(void *), PAGE_READWRITE, &old)) {
        MC2VR_LOG("waits: VirtualProtect failed on %s slot %p", name, (void *)slot);
        return false;
    }
    *orig = (T)*p;
    if (*orig == nullptr) {
        VirtualProtect(p, sizeof(void *), old, &old);
        MC2VR_LOG("waits: %s slot %p is empty — not patched", name, (void *)slot);
        return false;
    }
    *p = (void *)wrapper;
    VirtualProtect(p, sizeof(void *), old, &old);
    MC2VR_LOG("waits: %s slot %p -> wrapper (was %p)", name, (void *)slot, (void *)*orig);
    return true;
}

// ---- phase split ------------------------------------------------------------
constexpr int kMarks = 6;
double g_t[kMarks] = {};
bool g_have[kMarks] = {};
double g_prev_exit = 0.0;
// phases: 0 gate, 1 update, 2 pass1, 3 pass2, 4 tail, 5 outside
constexpr int kPhases = 6;
double g_phase_sum[kPhases] = {}, g_phase_max[kPhases] = {};
uint64_t g_phase_frames = 0;
uint64_t g_frames = 0;
uint64_t g_qpc_at_frame = 0;
uint64_t g_qpc_max_per_frame = 0;
DWORD g_mark_tid[kMarks] = {};  // thread that last set each mark

void close_frame()
{
    const bool full = g_have[0] && g_have[1] && g_have[2] && g_have[3] && g_have[4] && g_have[5];
    if (full && g_prev_exit > 0.0) {
        const double ph[kPhases] = {
            g_t[1] - g_t[0], g_t[2] - g_t[1], g_t[3] - g_t[2],
            g_t[4] - g_t[3], g_t[5] - g_t[4], g_t[0] - g_prev_exit,
        };
        for (int i = 0; i < kPhases; i++) {
            g_phase_sum[i] += ph[i];
            if (ph[i] > g_phase_max[i]) g_phase_max[i] = ph[i];
        }
        g_phase_frames++;
    }
    if (g_have[5]) g_prev_exit = g_t[5];
}

void print_top(const char *name, const Census &c, bool timed)
{
    if (c.calls == 0) return;
    char line[1024];
    int n = 0;
    // up to 4 callers, largest first
    bool used[kCallers] = {};
    for (int k = 0; k < 4; k++) {
        int best = -1;
        for (int i = 0; i < kCallers; i++) {
            if (used[i] || c.top[i].ret == 0) continue;
            const double key = timed ? c.top[i].ms : (double)c.top[i].calls;
            const double bk = best < 0 ? -1.0 : (timed ? c.top[best].ms : (double)c.top[best].calls);
            if (key > bk) best = i;
        }
        if (best < 0) break;
        used[best] = true;
        const Caller &b = c.top[best];
        n += _snprintf(line + n, sizeof(line) - n, " %08x:%u/%.1fms(arg<=%u)",
                       (unsigned)b.ret, b.calls, b.ms, b.arg_max);
        if (timed && g_frames && n > 0 && n < (int)sizeof(line)) {
            n += _snprintf(line + n, sizeof(line) - n, "[");
            for (int ph = 0; ph < 6 && n > 0 && n < (int)sizeof(line); ph++) {
                if (b.phase_ms[ph] > 0.0)
                    n += _snprintf(line + n, sizeof(line) - n, "%s%s %.2f", ph ? " " : "", kPhaseNames[ph],
                                   b.phase_ms[ph] / (double)g_frames);
            }
            if (n > 0 && n < (int)sizeof(line)) n += _snprintf(line + n, sizeof(line) - n, " ms/frame]");
        }
        if (n < 0 || n >= (int)sizeof(line)) { n = (int)sizeof(line) - 1; break; }
    }
    line[n > 0 ? n : 0] = 0;
    MC2VR_LOG("waits: %s calls=%llu (%.1f/frame) total=%.1fms (%.2fms/frame) top:%s", name,
              (unsigned long long)c.calls,
              g_frames ? (double)c.calls / (double)g_frames : 0.0, c.ms,
              g_frames ? c.ms / (double)g_frames : 0.0, line);
}

} // namespace

bool set_enabled(const char *value)
{
    if (strcmp(value, "on") == 0) g_enabled = true;
    else if (strcmp(value, "off") == 0) g_enabled = false;
    else return false;
    MC2VR_LOG("waits: debug_wait_census=%s", value);
    return true;
}

void install()
{
    if (!g_enabled || g_installed) return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpc_to_ms = 1000.0 / (double)f.QuadPart;
    g_main_tid = GetCurrentThreadId();  // first Present: the device/main thread
    patch(IAT_SLEEP, &w_sleep, &o_sleep, "Sleep");
    patch(IAT_SLEEPEX, &w_sleepex, &o_sleepex, "SleepEx");
    patch(IAT_WFSO, &w_wfso, &o_wfso, "WaitForSingleObject");
    patch(IAT_QPC, &w_qpc, &o_qpc, "QueryPerformanceCounter");
    patch(IAT_TIMEGETTIME, &w_tgt, &o_tgt, "timeGetTime");
    g_installed = true;
    MC2VR_LOG("waits: census installed (main thread %lu)", (unsigned long)g_main_tid);
}

void mark(Mark m)
{
    if (!g_installed) return;
    const int i = (int)m;
    if (m == Mark::FrameEntry) {
        close_frame();
        for (bool &h : g_have) h = false;
        g_frames++;
        g_update_tid = GetCurrentThreadId();
        const uint64_t qc = g_qpc[0].calls + g_qpc[1].calls;
        const uint64_t q = qc - g_qpc_at_frame;
        if (q > g_qpc_max_per_frame) g_qpc_max_per_frame = q;
        g_qpc_at_frame = qc;
    }
    g_t[i] = now_ms();
    g_have[i] = true;
    g_mark_tid[i] = GetCurrentThreadId();
    // Phase the main thread enters at this mark.
    static const int kNext[kMarks] = {0, 1, 2, 3, 4, 5};
    g_cur_phase = kNext[i];
}

void report_window()
{
    if (!g_installed) return;
    if (g_phase_frames) {
        const double n = (double)g_phase_frames;
        MC2VR_LOG("waits: phases avg/max ms over %llu frames | gate %.2f/%.2f | update %.2f/%.2f | "
                  "pass1 %.2f/%.2f | pass2 %.2f/%.2f | tail %.2f/%.2f | outside %.2f/%.2f",
                  (unsigned long long)g_phase_frames,
                  g_phase_sum[0] / n, g_phase_max[0], g_phase_sum[1] / n, g_phase_max[1],
                  g_phase_sum[2] / n, g_phase_max[2], g_phase_sum[3] / n, g_phase_max[3],
                  g_phase_sum[4] / n, g_phase_max[4], g_phase_sum[5] / n, g_phase_max[5]);
    }
    MC2VR_LOG("waits: threads — census(first Present) %lu | FrameTick entry/exit %lu/%lu | "
              "pass1/pass2/end %lu/%lu/%lu",
              (unsigned long)g_main_tid, (unsigned long)g_mark_tid[0], (unsigned long)g_mark_tid[5],
              (unsigned long)g_mark_tid[2], (unsigned long)g_mark_tid[3], (unsigned long)g_mark_tid[4]);
    static const char *const kThread[2] = {"render", "update"};
    for (int w = 0; w < 2; w++) {
        char name[48];
        _snprintf(name, sizeof name, "%s Sleep", kThread[w]); name[sizeof name - 1] = 0;
        print_top(name, g_sleep[w], true);
        _snprintf(name, sizeof name, "%s SleepEx", kThread[w]); name[sizeof name - 1] = 0;
        print_top(name, g_sleepex[w], true);
        _snprintf(name, sizeof name, "%s WaitForSingleObject", kThread[w]); name[sizeof name - 1] = 0;
        print_top(name, g_wfso[w], true);
        _snprintf(name, sizeof name, "%s QPC", kThread[w]); name[sizeof name - 1] = 0;
        print_top(name, g_qpc[w], false);
        _snprintf(name, sizeof name, "%s timeGetTime", kThread[w]); name[sizeof name - 1] = 0;
        print_top(name, g_tgt[w], false);
    }
    MC2VR_LOG("waits: QPC max calls in one frame = %llu (a spin-wait shows as thousands)",
              (unsigned long long)g_qpc_max_per_frame);
    for (int w = 0; w < 2; w++) {
        g_sleep[w].reset(); g_sleepex[w].reset(); g_wfso[w].reset(); g_qpc[w].reset(); g_tgt[w].reset();
    }
    for (int i = 0; i < kPhases; i++) g_phase_sum[i] = g_phase_max[i] = 0.0;
    g_phase_frames = 0;
    g_frames = 0;
    g_qpc_at_frame = 0;
    g_qpc_max_per_frame = 0;
}

} // namespace mc2vr::wait_census
