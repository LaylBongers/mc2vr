// Scene -> draw-record build census — see build_census.hpp.

#include "build_census.hpp"

#include <safetyhook.hpp>

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

#include "game_addresses.h"
#include "hooks.hpp"
#include "log.hpp"

namespace mc2vr::build_census {

namespace {

bool g_enabled = false;

int64_t qpc_now()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);  // the carrier's own import, not the game's slot
    return t.QuadPart;
}

double g_qpc_to_ms = 0.0;

// ---- events -----------------------------------------------------------------
enum Ev : uint8_t {
    EV_TASK_BEGIN = 0,  // node 0 entry
    EV_TASK_BUILD,      // node 3 entry
    EV_BUILD_IN,        // RenderShell_BuildSceneRecords entry
    EV_BUILD_OUT,       // its loop done
    EV_TASK_PUBLISH,    // node 4 entry
    EV_PUB_IN,          // call PgPrimitive_PublishFrame
    EV_PUB_OUT,         // returned
    EV_TASK_RENDER,     // node 2 entry (RenderTask_RenderFrame)
    EV_COUNT
};
const char *const EV_NAME[EV_COUNT] = {"task0-begin", "task3-build", "build-in",  "build-out",
                                       "task4-pub",   "pub-in",      "pub-out",   "task2-render"};

constexpr int TID_SLOTS = 4;
struct EvStats {
    volatile LONG n;
    volatile LONG tid[TID_SLOTS];  // first distinct thread IDs seen this window
};
EvStats g_ev[EV_COUNT];

volatile DWORD g_render_tid = 0;  // last RenderTask_RenderFrame thread

// One-shot ordered sequence capture, armed after the first window.
struct SeqRec {
    uint8_t ev;
    uint32_t tid;
    uint32_t frame;
    int64_t qpc;
};
constexpr LONG SEQ_MAX = 160;
SeqRec g_seq[SEQ_MAX];
volatile LONG g_seq_n = 0;
volatile LONG g_seq_armed = 0;  // 0 = not yet, 1 = capturing, 2 = dumped

void note_tid(EvStats &s, DWORD tid)
{
    for (int i = 0; i < TID_SLOTS; i++) {
        const LONG cur = s.tid[i];
        if ((DWORD)cur == tid) {
            return;
        }
        if (cur == 0 && InterlockedCompareExchange(&s.tid[i], (LONG)tid, 0) == 0) {
            return;
        }
        if ((DWORD)s.tid[i] == tid) {
            return;
        }
    }
}

void event(Ev ev, int64_t now)
{
    const DWORD tid = GetCurrentThreadId();
    InterlockedIncrement(&g_ev[ev].n);
    note_tid(g_ev[ev], tid);
    if (g_seq_armed == 1) {
        const LONG i = InterlockedIncrement(&g_seq_n) - 1;
        if (i < SEQ_MAX) {
            g_seq[i] = {(uint8_t)ev, (uint32_t)tid, (uint32_t)hooks::frame_count(), now};
        }
    }
}

// ---- build / publish timing (each runs on one thread at a time) --------------
volatile DWORD g_build_tid = 0;
volatile LONG g_build_active = 0;
int64_t g_build_t0 = 0;
uint32_t g_build_n = 0;
int64_t g_build_sum = 0, g_build_max = 0;
double g_engine_ms_sum = 0.0;  // g_SceneBuildMs sampled at build entry (= previous build)
uint32_t g_engine_ms_n = 0;
uint32_t g_views_sum = 0;

int64_t g_pub_t0 = 0;
uint32_t g_pub_n = 0;
int64_t g_pub_sum = 0, g_pub_max = 0;
uint64_t g_pub_records_sum = 0;
uint32_t g_pub_records_max = 0;

// ---- emitter tally -------------------------------------------------------------
// key = ret << 32 | view << 16 | type << 8 | 1 (nonzero = used). view 0xffff =
// submitted outside the build bracket (or from another thread).
constexpr uint32_t TALLY_SIZE = 4096;  // power of two
volatile LONG64 g_tally_key[TALLY_SIZE];
volatile LONG g_tally_count[TALLY_SIZE];
volatile LONG g_tally_overflow = 0;
volatile LONG g_submit_n = 0;
volatile LONG g_submit_in_build = 0;

void tally(uint64_t key)
{
    uint32_t h = (uint32_t)(key ^ (key >> 29)) * 0x9e3779b1u;
    for (uint32_t probe = 0; probe < 64; probe++) {
        const uint32_t i = (h + probe) & (TALLY_SIZE - 1);
        const uint64_t cur = (uint64_t)InterlockedCompareExchange64(&g_tally_key[i], (LONG64)key, 0);
        if (cur == 0 || cur == key) {
            InterlockedIncrement(&g_tally_count[i]);
            return;
        }
    }
    InterlockedIncrement(&g_tally_overflow);
}

// ---- handlers ------------------------------------------------------------------
uint16_t rs_u16(uintptr_t off)
{
    return *(volatile const uint16_t *)(MC2_G_RENDERSHELL + off);
}

void h_task_begin(safetyhook::Context &) { event(EV_TASK_BEGIN, qpc_now()); }
void h_task_build(safetyhook::Context &) { event(EV_TASK_BUILD, qpc_now()); }
void h_task_publish(safetyhook::Context &) { event(EV_TASK_PUBLISH, qpc_now()); }

void h_task_render(safetyhook::Context &)
{
    g_render_tid = GetCurrentThreadId();
    event(EV_TASK_RENDER, qpc_now());
}

void h_build_in(safetyhook::Context &)
{
    const int64_t now = qpc_now();
    event(EV_BUILD_IN, now);
    g_views_sum += rs_u16(MC2_RS_SUBVIEW_COUNT_OFF);
    const float engine_ms = *(volatile const float *)MC2_G_SCENE_BUILD_MS;
    if (engine_ms > 0.0f && engine_ms < 1000.0f) {
        g_engine_ms_sum += engine_ms;
        g_engine_ms_n++;
    }
    g_build_tid = GetCurrentThreadId();
    g_build_t0 = now;
    InterlockedExchange(&g_build_active, 1);
}

void h_build_out(safetyhook::Context &)
{
    const int64_t now = qpc_now();
    InterlockedExchange(&g_build_active, 0);
    event(EV_BUILD_OUT, now);
    if (g_build_t0 != 0) {
        const int64_t d = now - g_build_t0;
        g_build_sum += d;
        g_build_max = std::max(g_build_max, d);
        g_build_n++;
        g_build_t0 = 0;
    }
}

void h_pub_in(safetyhook::Context &)
{
    const int64_t now = qpc_now();
    event(EV_PUB_IN, now);
    const uint32_t records = *(volatile const uint32_t *)MC2_G_PRIM_WRITE_COUNT;
    g_pub_records_sum += records;
    g_pub_records_max = std::max(g_pub_records_max, records);
    g_pub_t0 = now;
}

void h_pub_out(safetyhook::Context &)
{
    const int64_t now = qpc_now();
    event(EV_PUB_OUT, now);
    if (g_pub_t0 != 0) {
        const int64_t d = now - g_pub_t0;
        g_pub_sum += d;
        g_pub_max = std::max(g_pub_max, d);
        g_pub_n++;
        g_pub_t0 = 0;
    }
}

void h_submit(safetyhook::Context &ctx)
{
    const uint32_t ret = *(volatile const uint32_t *)ctx.esp;  // MidHook at the entry instruction
    const uint8_t type = *(volatile const uint8_t *)MC2_G_PRIM_SCRATCH_TYPE;
    uint16_t view = 0xffff;
    if (g_build_active && GetCurrentThreadId() == g_build_tid) {
        view = rs_u16(MC2_RS_SUBVIEW_CUR_OFF);
        InterlockedIncrement(&g_submit_in_build);
    }
    InterlockedIncrement(&g_submit_n);
    tally(((uint64_t)ret << 32) | ((uint64_t)view << 16) | ((uint64_t)type << 8) | 1u);
}

struct Site {
    const char *name;
    uintptr_t addr;
    void (*fn)(safetyhook::Context &);
};
const Site SITES[] = {
    {"RenderTask_BeginFrame", MC2_RENDERTASK_BEGINFRAME, h_task_begin},
    {"RenderTask_RenderFrame", MC2_RENDERTASK_RENDERFRAME, h_task_render},
    {"RenderTask_BuildScene", MC2_RENDERTASK_BUILDSCENE, h_task_build},
    {"RenderTask_PublishRecords", MC2_RENDERTASK_PUBLISH, h_task_publish},
    {"BuildSceneRecords entry", MC2_BUILDSCENE_ENTRY, h_build_in},
    {"BuildSceneRecords loop done", MC2_BUILDSCENE_LOOP_DONE, h_build_out},
    {"PublishFrame call", MC2_PUBLISH_CALL, h_pub_in},
    {"PublishFrame return", MC2_PUBLISH_RETURN, h_pub_out},
    {"PgPrimitiveScratchRecord_Submit", MC2_PRIM_SCRATCH_SUBMIT, h_submit},
};
constexpr size_t SITE_COUNT = sizeof(SITES) / sizeof(SITES[0]);
SafetyHookMid g_mid[SITE_COUNT];

uint64_t g_window_frame0 = 0;
bool g_first_window_done = false;

char tid_label(DWORD tid)
{
    if (tid == 0) {
        return '?';
    }
    if (tid == hooks::update_thread_id()) {
        return 'U';
    }
    if (tid == g_render_tid) {
        return 'R';
    }
    return 'o';
}

void dump_sequence()
{
    const LONG n = std::min((LONG)g_seq_n, SEQ_MAX);
    MC2VR_LOG("build: sequence (%ld events; U = update thread %lu, R = render-task thread %lu, "
              "o = other; t = ms since first event)",
              (long)n, (unsigned long)hooks::update_thread_id(), (unsigned long)g_render_tid);
    const int64_t t0 = n > 0 ? g_seq[0].qpc : 0;
    for (LONG i = 0; i < n; i++) {
        const SeqRec &r = g_seq[i];
        MC2VR_LOG("build:   %3ld t=%8.3f frame=%lu tid=%lu[%c] %s", (long)i,
                  (double)(r.qpc - t0) * g_qpc_to_ms, (unsigned long)r.frame, (unsigned long)r.tid,
                  tid_label(r.tid), EV_NAME[r.ev]);
    }
}

struct Row {
    uint64_t key;
    uint32_t count;
};
Row g_rows[TALLY_SIZE];

} // namespace

bool set_enabled(const char *value)
{
    if (strcmp(value, "on") == 0) {
        g_enabled = true;
    } else if (strcmp(value, "off") == 0) {
        g_enabled = false;
    } else {
        return false;
    }
    MC2VR_LOG("build: debug_build_census = %s", value);
    return true;
}

void install()
{
    if (!g_enabled) {
        return;
    }
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpc_to_ms = 1000.0 / (double)f.QuadPart;
    unsigned ok = 0;
    for (size_t i = 0; i < SITE_COUNT; i++) {
        auto mid = SafetyHookMid::create(reinterpret_cast<uint8_t *>(SITES[i].addr), SITES[i].fn);
        if (!mid) {
            MC2VR_LOG("build: hook %s @ %p failed (error %u) — skipped", SITES[i].name,
                      (void *)SITES[i].addr, (unsigned)mid.error().type);
            continue;
        }
        g_mid[i] = std::move(*mid);
        ok++;
    }
    g_window_frame0 = hooks::frame_count();
    MC2VR_LOG("build: %u/%u census hooks installed", ok, (unsigned)SITE_COUNT);
}

void report_window()
{
    if (!g_enabled) {
        return;
    }
    const uint64_t frame = hooks::frame_count();
    const uint64_t frames = frame > g_window_frame0 ? frame - g_window_frame0 : 0;
    g_window_frame0 = frame;
    const double fr = frames ? (double)frames : 1.0;

    // Events: counts per frame and the threads they ran on.
    char line[512];
    int n = snprintf(line, sizeof(line), "build: window %llu frames | per frame:", (unsigned long long)frames);
    for (int e = 0; e < EV_COUNT; e++) {
        const LONG c = InterlockedExchange(&g_ev[e].n, 0);
        n += snprintf(line + n, sizeof(line) - n, " %s=%.2f", EV_NAME[e], (double)c / fr);
    }
    MC2VR_LOG("%s", line);
    n = snprintf(line, sizeof(line), "build: threads (U=update %lu, R=render-task %lu):",
                 (unsigned long)hooks::update_thread_id(), (unsigned long)g_render_tid);
    for (int e = 0; e < EV_COUNT; e++) {
        n += snprintf(line + n, sizeof(line) - n, " %s=", EV_NAME[e]);
        for (int k = 0; k < TID_SLOTS; k++) {
            const DWORD t = (DWORD)InterlockedExchange(&g_ev[e].tid[k], 0);
            if (t != 0) {
                n += snprintf(line + n, sizeof(line) - n, "%lu[%c]", (unsigned long)t, tid_label(t));
            }
        }
    }
    MC2VR_LOG("%s", line);

    // Timing.
    MC2VR_LOG("build: BuildSceneRecords n=%u avg=%.3fms max=%.3fms | engine g_SceneBuildMs avg=%.3fms "
              "(n=%u) | sub-views/build=%.2f",
              g_build_n, g_build_n ? (double)g_build_sum / g_build_n * g_qpc_to_ms : 0.0,
              (double)g_build_max * g_qpc_to_ms, g_engine_ms_n ? g_engine_ms_sum / g_engine_ms_n : 0.0,
              g_engine_ms_n, g_build_n ? (double)g_views_sum / g_build_n : 0.0);
    MC2VR_LOG("build: PublishFrame n=%u avg=%.3fms max=%.3fms | records/publish avg=%.0f max=%u",
              g_pub_n, g_pub_n ? (double)g_pub_sum / g_pub_n * g_qpc_to_ms : 0.0,
              (double)g_pub_max * g_qpc_to_ms, g_pub_n ? (double)g_pub_records_sum / g_pub_n : 0.0,
              g_pub_records_max);
    g_build_n = 0;
    g_build_sum = g_build_max = 0;
    g_engine_ms_sum = 0.0;
    g_engine_ms_n = 0;
    g_views_sum = 0;
    g_pub_n = 0;
    g_pub_sum = g_pub_max = 0;
    g_pub_records_sum = 0;
    g_pub_records_max = 0;

    // Emitter tally (snapshot + reset counts; keys persist).
    const LONG submits = InterlockedExchange(&g_submit_n, 0);
    const LONG in_build = InterlockedExchange(&g_submit_in_build, 0);
    const LONG overflow = InterlockedExchange(&g_tally_overflow, 0);
    uint32_t rows = 0;
    for (uint32_t i = 0; i < TALLY_SIZE; i++) {
        const LONG c = InterlockedExchange(&g_tally_count[i], 0);
        if (c > 0) {
            g_rows[rows++] = {(uint64_t)g_tally_key[i], (uint32_t)c};
        }
    }
    std::sort(g_rows, g_rows + rows, [](const Row &a, const Row &b) { return a.count > b.count; });
    MC2VR_LOG("build: submits=%.1f/frame (in build bracket %.1f%%) | %u distinct (emitter, view, type)%s",
              (double)submits / fr, submits ? 100.0 * in_build / submits : 0.0, rows,
              overflow ? " | TALLY OVERFLOW" : "");
    const uint32_t show = std::min(rows, 40u);
    for (uint32_t i = 0; i < show; i++) {
        const uint64_t k = g_rows[i].key;
        const unsigned view = (unsigned)((k >> 16) & 0xffff);
        char vbuf[16];
        if (view == 0xffff) {
            snprintf(vbuf, sizeof(vbuf), "--");
        } else {
            snprintf(vbuf, sizeof(vbuf), "%u", view);
        }
        MC2VR_LOG("build:   emit ret=%08x view=%-3s type=%-2u  %.2f/frame", (unsigned)(k >> 32), vbuf,
                  (unsigned)((k >> 8) & 0xff), (double)g_rows[i].count / fr);
    }

    // Sequence capture: arm after the first window (steady state), dump once full.
    if (!g_first_window_done) {
        g_first_window_done = true;
        InterlockedExchange(&g_seq_armed, 1);
    } else if (g_seq_armed == 1 && g_seq_n >= SEQ_MAX) {
        InterlockedExchange(&g_seq_armed, 2);
        dump_sequence();
    }
}

} // namespace mc2vr::build_census
