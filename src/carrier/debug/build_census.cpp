// Scene -> draw-record build census — see build_census.hpp.

#include "build_census.hpp"

#include <safetyhook.hpp>

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

#include "game/addresses.h"
#include "game/pg_primitive.hpp"
#include "hooks/site.hpp"
#include "hooks/frame_tick.hpp"
#include "log.hpp"

namespace mc2vr::build_census {

namespace {

bool g_enabled = false;
bool g_installed = false;  // the hooks are in (its publish tap only counts then)

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

// ---- GUI (Scaleform GFx 2.x GRenderer implementation, vtable 0x00bad178) ------
// A record is a GUI record iff PgPrimitiveScratchRecord_Submit's caller lies in
// the renderer's code range (every scene emitter is outside it).
bool is_gui_ret(uint32_t ret) { return ret >= MC2_GUI_RENDERER_CODE_LO && ret < MC2_GUI_RENDERER_CODE_HI; }

volatile LONG g_gui_displays = 0;  // BeginDisplay calls (window)
volatile LONG g_gui_records = 0;   // GUI records submitted (window)
int64_t g_gui_t_first = 0, g_gui_t_last = 0;  // this build: first BeginDisplay .. last GUI submit
int64_t g_gui_span_sum = 0;
uint32_t g_gui_span_n = 0;
uint32_t g_gui_first_index_sum = 0;  // record index (write count) at the first BeginDisplay
uint32_t g_gui_first_index_n = 0;
uint32_t g_gui_bits[0x2000 / 32];  // GUI record indices of the buffer being built
// Snapshot at PublishFrame return: the GUI marks of the list about to be drawn
// (the next build clears g_gui_bits while that list is still drawing).
bool g_gui_marks_wanted = false;  // debug_gui_census
uint32_t g_pub_gui_bits[0x2000 / 32];
// Each record's emitter: the return address of its PgPrimitiveScratchRecord_Submit
// call (record index = the write count at entry), and the published snapshot.
uint32_t g_emitters[0x2000];
uint32_t g_pub_emitters[0x2000];
// Further up (heuristic): the next stack dwords above the return address
// that point just after a call instruction in .text — direct (E8) or
// indirect (FF /2: register, [reg], [reg+disp8/32], [abs32]).
constexpr int kAbove = 3;
uint32_t g_emitters2[0x2000][kAbove];
uint32_t g_pub_emitters2[0x2000][kAbove];

bool is_return_address(uint32_t v)
{
    if (v < 0x00401006u || v > 0x00b04fffu) return false;
    const volatile uint8_t *p = reinterpret_cast<const volatile uint8_t *>(v);
    if (p[-5] == 0xE8) return true;                                       // call rel32
    if (p[-2] == 0xFF && (p[-1] & 0xF8) == 0xD0) return true;             // call reg
    if (p[-2] == 0xFF && (p[-1] & 0xF8) == 0x10 && p[-1] != 0x14 && p[-1] != 0x15) return true;  // call [reg]
    if (p[-3] == 0xFF && (p[-2] & 0xF8) == 0x50 && p[-2] != 0x54) return true;  // call [reg+disp8]
    if (p[-6] == 0xFF && ((p[-5] & 0xF8) == 0x90 || p[-5] == 0x15)) return true;  // [reg+disp32] / [abs32]
    return false;
}

void callers_above(uint32_t ret, uint32_t esp, uint32_t out[kAbove])
{
    int n = 0;
    for (uint32_t a = esp + 4, end = esp + 4 + 96 * 4; a < end && n < kAbove; a += 4) {
        const uint32_t v = *(volatile const uint32_t *)a;
        if (v != ret && is_return_address(v)) out[n++] = v;
    }
    for (; n < kAbove; n++) out[n] = 0;
}
const void *volatile g_pub_base = nullptr;

constexpr int BD_SEEN_MAX = 6;  // one-shot BeginDisplay caller log
uint32_t g_bd_seen[BD_SEEN_MAX];
int g_bd_seen_n = 0;

// Published-list analysis, requested by the poller, run at PublishFrame return
// on the render thread, printed by the next report.
struct Combo {
    uint64_t key;  // viewType | viewContextIdx | screenIdx | rtWriteMask | stencilMode | viewIdx
    uint32_t n;
};
constexpr int COMBO_MAX = 10;
struct ListStats {
    uint32_t walked, gui, first_rank, last_rank, scene_after_last_gui;
    uint64_t gui_key_min, gui_key_max, scene_key_min, scene_key_max;
    Combo gui_combo[COMBO_MAX], scene_combo[COMBO_MAX];
    int gui_combo_n, scene_combo_n;
    uint32_t vs_count;
    float vs[8];
    uint32_t gui_with_cmd, gui_with_vs;
};
ListStats g_list;
volatile LONG g_list_request = 0;  // poller -> render thread
volatile LONG g_list_ready = 0;    // render thread -> poller

void combo_add(Combo *c, int *n, uint64_t key)
{
    for (int i = 0; i < *n; i++) {
        if (c[i].key == key) {
            c[i].n++;
            return;
        }
    }
    if (*n < COMBO_MAX) {
        c[(*n)++] = {key, 1};
    }
}

void analyse_published_list()
{
    ListStats st = {};
    st.first_rank = UINT32_MAX;
    st.gui_key_min = st.scene_key_min = UINT64_MAX;
    using namespace game;
    const PgPrimitive *base = g_PrimitiveBase::get();
    uint32_t idx = g_PrimitiveHead::get();
    for (uint32_t rank = 0; idx != kPrimListEnd && idx < kPrimitivesPerBuffer && rank < kPrimitivesPerBuffer;
         rank++) {
        const PgPrimitive &rec = base[idx];
        const PrimSortEntry &se = g_PrimitiveSortKeys::at(idx);
        const uint64_t sort_key = (uint64_t)se.key[0] | ((uint64_t)se.key[1] << 16) | ((uint64_t)se.key[2] << 32);
        const uint64_t combo = (uint64_t)rec.viewType | ((uint64_t)rec.viewContextIdx << 8) |
                               ((uint64_t)rec.screenIdx << 16) | ((uint64_t)rec.rtWriteMask << 32) |
                               ((uint64_t)rec.stencilMode << 40) | ((uint64_t)rec.viewIdx << 48);
        const bool gui = (g_gui_bits[idx >> 5] >> (idx & 31)) & 1u;
        if (gui) {
            st.gui++;
            st.first_rank = std::min(st.first_rank, rank);
            st.last_rank = rank;
            st.scene_after_last_gui = 0;
            st.gui_key_min = std::min(st.gui_key_min, sort_key);
            st.gui_key_max = std::max(st.gui_key_max, sort_key);
            combo_add(st.gui_combo, &st.gui_combo_n, combo);
            const uint32_t vcount = rec.vsConstCount;
            const float *vdata = rec.vsConstData;
            if (rec.cmdStream != nullptr) {
                st.gui_with_cmd++;
            }
            if (vcount != 0 && vdata != nullptr) {
                st.gui_with_vs++;
                if (st.vs_count == 0 && vcount <= 256 && !IsBadReadPtr(vdata, 8 * sizeof(float))) {
                    st.vs_count = vcount;
                    memcpy(st.vs, vdata, sizeof(st.vs));
                }
            }
        } else {
            st.scene_after_last_gui++;
            st.scene_key_min = std::min(st.scene_key_min, sort_key);
            st.scene_key_max = std::max(st.scene_key_max, sort_key);
            combo_add(st.scene_combo, &st.scene_combo_n, combo);
        }
        st.walked++;
        idx = se.next;
    }
    if (st.gui == 0) {
        st.scene_after_last_gui = 0;
    }
    g_list = st;
    InterlockedExchange(&g_list_ready, 1);
}

// Heuristic backtrace: stack dwords in .text preceded by an
// E8 call, outermost last. Stale stack data can produce false links.
void log_chain(uint32_t ret, uint32_t esp)
{
    const uint32_t stack_top = (uint32_t)((NT_TIB *)NtCurrentTeb())->StackBase;
    const uint32_t limit = std::min(esp + 4 + 2048, stack_top);
    char line[512];
    int n = snprintf(line, sizeof(line), "%08x", ret);
    int found = 0;
    for (uint32_t a = esp + 4; a + 4 <= limit && found < 14; a += 4) {
        const uint32_t v = *(volatile const uint32_t *)a;
        if (v >= 0x00401006u && v <= 0x00b04fffu && v != ret && *(const volatile uint8_t *)(v - 5) == 0xE8 &&
            n < (int)sizeof(line) - 16) {
            n += snprintf(line + n, sizeof(line) - n, " <- %08x", v);
            found++;
        }
    }
    MC2VR_LOG("build: GUI BeginDisplay chain: %s", line);
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
    memset(g_gui_bits, 0, sizeof(g_gui_bits));
    g_gui_t_first = g_gui_t_last = 0;
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
    if (g_gui_t_first != 0 && g_gui_t_last > g_gui_t_first) {
        g_gui_span_sum += g_gui_t_last - g_gui_t_first;
        g_gui_span_n++;
    }
}

void h_pub_in(safetyhook::Context &)
{
    const int64_t now = qpc_now();
    event(EV_PUB_IN, now);
    const uint32_t records = game::g_PrimWriteCount::get();
    g_pub_records_sum += records;
    g_pub_records_max = std::max(g_pub_records_max, records);
    g_pub_t0 = now;
}

void pub_out()
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
    if (g_list_request && InterlockedExchange(&g_list_request, 0)) {
        analyse_published_list();
    }
    if (g_gui_marks_wanted) {
        g_pub_base = nullptr;
        memcpy(g_pub_gui_bits, g_gui_bits, sizeof(g_pub_gui_bits));
        memcpy(g_pub_emitters, g_emitters, sizeof(g_pub_emitters));
        memcpy(g_pub_emitters2, g_emitters2, sizeof(g_pub_emitters2));
        g_pub_base = game::g_PrimitiveBase::get();
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
    if (g_gui_marks_wanted) {
        const uint32_t slot = game::g_PrimWriteCount::get();
        if (slot < 0x2000) {
            g_emitters[slot] = ret;
            callers_above(ret, (uint32_t)ctx.esp, g_emitters2[slot]);
        }
    }
    if (is_gui_ret(ret)) {
        InterlockedIncrement(&g_gui_records);
        const uint32_t index = game::g_PrimWriteCount::get();  // this record's slot
        if (index < 0x2000) {
            g_gui_bits[index >> 5] |= 1u << (index & 31);
        }
        if (g_build_active) {
            g_gui_t_last = qpc_now();
        }
    }
    tally(((uint64_t)ret << 32) | ((uint64_t)view << 16) | ((uint64_t)type << 8) | 1u);
}

void h_gui_begin_display(safetyhook::Context &ctx)
{
    const int64_t now = qpc_now();
    InterlockedIncrement(&g_gui_displays);
    if (g_build_active && g_gui_t_first == 0) {
        g_gui_t_first = now;
        g_gui_first_index_sum += game::g_PrimWriteCount::get();
        g_gui_first_index_n++;
    }
    // thiscall: ECX = renderer; [esp] ret, +4 GColor bg, +8 GViewport*, +0xc..+0x18 x0 x1 y0 y1.
    const uint32_t *sp = (const uint32_t *)ctx.esp;
    const uint32_t ret = sp[0];
    for (int i = 0; i < g_bd_seen_n; i++) {
        if (g_bd_seen[i] == ret) {
            return;
        }
    }
    if (g_bd_seen_n >= BD_SEEN_MAX) {
        return;
    }
    g_bd_seen[g_bd_seen_n++] = ret;
    const int32_t *vp = (const int32_t *)sp[2];
    const float *f = (const float *)&sp[3];
    MC2VR_LOG("build: GUI BeginDisplay #%d from %08x this=%08x bg=%08x viewport{buf %dx%d, rect %d,%d %dx%d} "
              "frame x %.1f..%.1f y %.1f..%.1f | build=%d sub-view=%u",
              g_bd_seen_n, ret, (unsigned)ctx.ecx, sp[1], vp ? vp[0] : -1, vp ? vp[1] : -1, vp ? vp[2] : -1,
              vp ? vp[3] : -1, vp ? vp[4] : -1, vp ? vp[5] : -1, f[0], f[1], f[2], f[3], (int)g_build_active,
              (unsigned)rs_u16(MC2_RS_SUBVIEW_CUR_OFF));
    log_chain(ret, ctx.esp);
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
    {"PgPrimitiveScratchRecord_Submit", MC2_PRIM_SCRATCH_SUBMIT, h_submit},
    {"GUI GRenderer::BeginDisplay", MC2_GUI_BEGIN_DISPLAY, h_gui_begin_display},
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

void on_publish_return()
{
    if (g_installed) {
        pub_out();
    }
}

void require_gui_marks()
{
    g_gui_marks_wanted = true;
}

uint32_t record_emitter(const void *base, uint32_t index, uint32_t above[3])
{
    if (above) above[0] = above[1] = above[2] = 0;
    if (!g_gui_marks_wanted || base == nullptr || base != g_pub_base || index >= 0x2000) {
        return 0;
    }
    if (above) {
        for (int i = 0; i < kAbove; i++) above[i] = g_pub_emitters2[index][i];
    }
    return g_pub_emitters[index];
}

int is_gui_record(const void *base, uint32_t index)
{
    if (!g_gui_marks_wanted || base == nullptr || base != g_pub_base || index >= 0x2000) {
        return -1;
    }
    return (int)((g_pub_gui_bits[index >> 5] >> (index & 31)) & 1u);
}

void install()
{
    if (!g_enabled && !g_gui_marks_wanted) {
        return;
    }
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpc_to_ms = 1000.0 / (double)f.QuadPart;
    unsigned ok = 0;
    for (size_t i = 0; i < SITE_COUNT; i++) {
        if (!hooks::create_mid(g_mid[i], SITES[i].addr, SITES[i].fn, SITES[i].name)) {
            MC2VR_LOG("build: hook %s skipped", SITES[i].name);
            continue;
        }
        ok++;
    }
    g_window_frame0 = hooks::frame_count();
    g_installed = true;
    MC2VR_LOG("build: %u/%u census hooks installed (+ the PublishFrame-return tap, hooks/hud.cpp)", ok,
              (unsigned)SITE_COUNT);
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

    // GUI (Scaleform renderer) summary.
    const LONG gui_displays = InterlockedExchange(&g_gui_displays, 0);
    const LONG gui_records = InterlockedExchange(&g_gui_records, 0);
    MC2VR_LOG("build: GUI BeginDisplay=%.2f/frame records=%.1f/frame | span first BeginDisplay..last GUI "
              "submit avg=%.3fms (n=%u) | first GUI record at write index avg=%.0f",
              (double)gui_displays / fr, (double)gui_records / fr,
              g_gui_span_n ? (double)g_gui_span_sum / g_gui_span_n * g_qpc_to_ms : 0.0, g_gui_span_n,
              g_gui_first_index_n ? (double)g_gui_first_index_sum / g_gui_first_index_n : 0.0);
    g_gui_span_sum = 0;
    g_gui_span_n = 0;
    g_gui_first_index_sum = 0;
    g_gui_first_index_n = 0;
    if (g_list_ready && InterlockedExchange(&g_list_ready, 0)) {
        const ListStats &st = g_list;
        MC2VR_LOG("build: list walked=%u gui=%u | GUI draw ranks %u..%u (%s), scene records after last GUI=%u | "
                  "sort keys GUI %012llx..%012llx scene %012llx..%012llx | GUI with cmdStream=%u vsConst=%u",
                  st.walked, st.gui, st.gui ? st.first_rank : 0, st.last_rank,
                  st.gui && st.last_rank - st.first_rank + 1 == st.gui ? "contiguous" : "interleaved",
                  st.scene_after_last_gui, (unsigned long long)(st.gui ? st.gui_key_min : 0),
                  (unsigned long long)st.gui_key_max,
                  (unsigned long long)(st.scene_key_min == UINT64_MAX ? 0 : st.scene_key_min),
                  (unsigned long long)st.scene_key_max, st.gui_with_cmd, st.gui_with_vs);
        for (int pass = 0; pass < 2; pass++) {
            const Combo *c = pass ? st.scene_combo : st.gui_combo;
            const int cn = pass ? st.scene_combo_n : st.gui_combo_n;
            for (int i = 0; i < cn; i++) {
                const uint64_t k = c[i].key;
                MC2VR_LOG("build:   %s fields viewType=%u viewContextIdx=%u screenIdx=%u rtWriteMask=%02x "
                          "stencilMode=%u viewIdx=%u  n=%u",
                          pass ? "scene" : "GUI  ", (unsigned)(k & 0xff), (unsigned)((k >> 8) & 0xff),
                          (unsigned)((k >> 16) & 0xffff), (unsigned)((k >> 32) & 0xff),
                          (unsigned)((k >> 40) & 0xff), (unsigned)(k >> 48), c[i].n);
            }
        }
        if (st.vs_count) {
            MC2VR_LOG("build:   first GUI record vsConstCount=%u data %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f",
                      st.vs_count, st.vs[0], st.vs[1], st.vs[2], st.vs[3], st.vs[4], st.vs[5], st.vs[6],
                      st.vs[7]);
        }
    }
    InterlockedExchange(&g_list_request, 1);

    // Sequence capture: arm once a window looks like gameplay (>1000 records/frame),
    // dump once full.
    if (!g_first_window_done) {
        if ((double)submits / fr > 1000.0) {
            g_first_window_done = true;
            InterlockedExchange(&g_seq_armed, 1);
        }
    } else if (g_seq_armed == 1 && g_seq_n >= SEQ_MAX) {
        InterlockedExchange(&g_seq_armed, 2);
        dump_sequence();
    }
}

} // namespace mc2vr::build_census
