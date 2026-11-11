#include "debug/render_dump.hpp"

#include <safetyhook.hpp>

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "vr/cull_frustum.hpp"
#include "game/addresses.h"
#include "hooks/site.hpp"
#include "vr/hud.hpp"
#include "vr/msaa.hpp"
#include "vr/world_markers.hpp"
#include "vr/eye_replay.hpp"
#include "vr/render_outputs.hpp"
#include "vr/render_size.hpp"
#include "hooks/frame_tick.hpp"
#include "log.hpp"
#include "build_census.hpp"
#include "stream_capture.hpp"
#include "vr/pacing.hpp"
#include "vr/view_rewrite.hpp"
#include "vr/camera_table.hpp"
#include "watch.hpp"
#include "crash_log.hpp"
#include "debug/camera_census.hpp"
#include "debug/gameplay_census.hpp"
#include "debug/gpu_census.hpp"
#include "debug/gui_census.hpp"
#include "debug/occlusion_off.hpp"
#include "vr/occlusion_queries.hpp"
#include "debug/ps_census.hpp"
#include "debug/wait_census.hpp"

namespace mc2vr::render {

namespace {

// ---- state (MidHook handlers: main thread; poller: own thread) ----------

constexpr uint32_t OPCODE_COUNT = 27; // cmp eax,0x1a
constexpr uint32_t VIEW_AGG_MAX = 96; // indices observed up to 82+ at runtime
constexpr uint32_t ENTRY_DUMP_MAX = 12;
constexpr uint32_t VIEW_STRIDE = 0x810;
constexpr uint32_t POLL_MS = 250;
constexpr uint32_t REPORT_POLLS = 10 * 1000 / POLL_MS;
constexpr uint32_t REFRESH_EVERY_WINDOWS = 3;  // re-dump one entry every ~30s
constexpr uint32_t REFRESH_MAX = 12;            // steady-state re-dumps per run

// Leaked by design (same teardown reasoning as the device hooks).
SafetyHookMid g_view_mid;
SafetyHookMid g_endofframe_mid; // FrameTick slot-4 call site
HANDLE g_poller_thread = nullptr;
volatile LONG g_poller_run = 0;

// Command histogram (window).
uint64_t g_cmd_hist[OPCODE_COUNT] = {};
uint64_t g_cmd_total = 0;

// View aggregation (window + one-shot dumps).
struct ViewAgg {
    uint32_t idx;
    uint32_t type;
    uint64_t count;
};
ViewAgg g_views[VIEW_AGG_MAX] = {};
uint32_t g_view_distinct = 0;
uint64_t g_view_submits = 0;      // total loop iterations
uint64_t g_view_frames = 0;       // frames with >=1 view submission
uint64_t g_agg_frame = UINT64_MAX;
uint32_t g_vpf_cur = 0;
uint32_t g_vpf_min = 0;
uint32_t g_vpf_max = 0;
uint32_t g_view_list_head = 0;    // DAT_00d29e60 = ACTIVE-VIEW LIST HEAD INDEX (not a count; link = ViewEntry+0x4, negative = end)
uint32_t g_dumps_done = 0;
uint32_t g_dumped_idx[ENTRY_DUMP_MAX] = {};

// Steady-state re-dumps: first-wave dumps fire on view activation (loading
// phase — entries pre-populated). The poller requests one re-dump per minute
// (rotating through observed indices); the MidHook performs it so the entry
// read happens mid-loop on the main thread.
volatile LONG g_refresh_target = -1;
bool g_view_dumps = false;  // debug_view_dumps=on
uint32_t g_refresh_done = 0;

// g_RenderShell slot 4/5 claim test.
uint64_t g_slot4_calls = 0;
bool g_slot4_logged = false;

// Queue counters (poller). prodA (base+0x10) is a RING POSITION, not a
// cumulative counter: observed values wrap inside 0..capacity-1 — track
// raw min/max/last per window, no delta arithmetic. prodB (base+0x14) read
// raw (observed constant 0 in gameplay so far).
uint32_t g_queue_elem = 0;
uint32_t g_queue_cap = 0;
uint32_t g_prod_a_last = 0, g_prod_a_min = 0xffffffff, g_prod_a_max = 0;
uint32_t g_prod_b_last = 0;
uint64_t g_queue_changed_polls = 0;

// ---- one-shot entry dumps (main thread, mid-loop: entry is stable) ------

void dump_bytes(const char *tag, uint32_t base_off, const uint8_t *bytes, uint32_t len)
{
    char line[96];
    for (uint32_t off = 0; off < len; off += 32) {
        uint32_t n = len - off < 32 ? len - off : 32;
        char *p = line;
        for (uint32_t i = 0; i < n; i++) {
            *p++ = "0123456789abcdef"[bytes[off + i] >> 4];
            *p++ = "0123456789abcdef"[bytes[off + i] & 0xf];
        }
        *p = '\0';
        MC2VR_LOG("%s +0x%03x: %s", tag, base_off + off, line);
    }
}

void dump_view_entry(uint32_t idx, uint32_t type, const uint8_t *entry, bool track)
{
    // Per-view object pointer at entry+0x7e4 (verified in disassembly).
    const uint32_t obj_ptr = *(const uint32_t *)(entry + MC2_VIEW_OBJ_PTR_OFF);
    const uint8_t t3 = *(const uint8_t *)(MC2_VIEW_TABLE3 + idx * 0x20 + 0x18);

    MC2VR_LOG("ViewDump%s idx=%u type=%u entry=%p obj=%p t3=%02x (%s)",
              track ? "" : " (refresh)", idx, type, entry, (const void *)(uintptr_t)obj_ptr,
              t3, track ? "first" : "steady-state");
    dump_bytes("entry", 0, entry, VIEW_STRIDE);
    if (obj_ptr) {
        dump_bytes("obj", 0, (const uint8_t *)(uintptr_t)obj_ptr, 0x40);
    }
    if (track) {
        g_dumped_idx[g_dumps_done++] = idx;
    }
}

// ---- MidHook handlers -----------------------------------------------------

// RenderCmd_ExecuteStream opcode dispatch: EAX = opcode. Also feeds the
// Stream tap (stream_capture.cpp): EBP/ESP identify the stream.
// RenderQueue_SubmitWorldPackets per-view loop head: ESI = view index,
// ECX = view type (WORD), EAX = byte offset. Entry = table + EAX.
void view_midhook(safetyhook::Context &ctx)
{
    const uint32_t idx = (uint32_t)ctx.esi;
    const uint32_t type = ctx.ecx & 0xffff;
    const uint8_t *entry = (const uint8_t *)(MC2_VIEW_TABLE + (uintptr_t)ctx.eax);

    g_view_submits++;

    // Culling-watch: identify the view entry whose camera fields get
    // hardware-watched (debug_watch; no-op when disabled). EBX = the frame-ctx
    // object (callee-saved from function entry — plate), needed for the
    // staged-block targets.
    watch::on_view(idx, type, entry, (uintptr_t)ctx.ebx);

    // Per-frame bookkeeping keyed on the FrameTick counter.
    const uint64_t frame = hooks::frame_count();
    if (frame != g_agg_frame) {
        if (g_agg_frame != UINT64_MAX && g_vpf_cur > 0) {
            g_view_frames++;
        }
        g_agg_frame = frame;
        g_vpf_cur = 0;
        g_view_list_head = *(const uint32_t *)0x00d29e60u; // active-view list head index (corrected; formerly mislabeled tableCount)
    }
    g_vpf_cur++;
    if (g_vpf_cur > g_vpf_max) {
        g_vpf_max = g_vpf_cur;
    }
    if (g_vpf_min == 0 || g_vpf_cur < g_vpf_min) {
        g_vpf_min = g_vpf_cur;
    }

    // Distinct (idx, type) aggregation — small linear scan.
    uint32_t i = 0;
    for (; i < g_view_distinct; i++) {
        if (g_views[i].idx == idx) {
            if (g_views[i].type != type) {
                g_views[i].type = type; // type changed — keep latest
            }
            g_views[i].count++;
            break;
        }
    }
    if (i == g_view_distinct && g_view_distinct < VIEW_AGG_MAX) {
        g_views[g_view_distinct++] = {idx, type, 1};
    }

    // One-shot dump per new index.
    if (g_view_dumps && g_dumps_done < ENTRY_DUMP_MAX) {
        bool seen = false;
        for (uint32_t d = 0; d < g_dumps_done; d++) {
            if (g_dumped_idx[d] == idx) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            dump_view_entry(idx, type, entry, true);
        }
    }

    // Steady-state re-dump requested by the poller (runs here so the entry
    // read stays on the main thread, mid-loop).
    if (g_view_dumps && g_refresh_target == (LONG)idx && g_refresh_done < REFRESH_MAX) {
        g_refresh_target = -1;
        g_refresh_done++;
        dump_view_entry(idx, type, entry, false);
    }
}

// ---- g_RenderShell slot 4/5 call sites ---------------------------------------
// MidHooks at GameShell_FrameTick's two per-frame virtual calls on
// g_RenderShell (slot 5 PostUpdateHook, slot 4 EndOfFrameHook — both the
// shared no-op VirtHook_NoOp). Installed at attach; they run at exactly the
// point the old vtable claim's handlers did (just before the no-op call),
// without depending on the object's vptr (no reinit-reinstall risk).

void endofframe_midhook(safetyhook::Context &)
{
    g_slot4_calls++;
    if (!g_slot4_logged) {
        g_slot4_logged = true;
        MC2VR_LOG("EndOfFrameHook call site (g_RenderShell slot 4) hit, frame=%llu",
                  (unsigned long long)hooks::frame_count());
    }
}

// ---- poller thread ------------------------------------------------------------

void report_window()
{
    // Keep the crash logger on top of any filter the game installed since.
    crash_log::reassert();

    // Views line.
    MC2VR_LOG("views: submits=%llu frames-with-views=%llu views/frame min=%u max=%u "
              "listHead=%u distinct=%u",
              (unsigned long long)g_view_submits, (unsigned long long)g_view_frames,
              g_vpf_min, g_vpf_max, g_view_list_head, g_view_distinct);
    if (g_view_distinct > 0) {
        char list[640];
        int n = 0;
        uint32_t shown = g_view_distinct < 12 ? g_view_distinct : 12;
        for (uint32_t i = 0; i < shown && n < (int)sizeof(list) - 32; i++) {
            n += _snprintf(list + n, sizeof(list) - n, " idx%u:t%u:%llu", g_views[i].idx,
                           g_views[i].type, (unsigned long long)g_views[i].count);
        }
        if (g_view_distinct > shown) {
            n += _snprintf(list + n, sizeof(list) - n, " ...(%u more)",
                           g_view_distinct - shown);
        }
        list[n] = '\0';
        MC2VR_LOG("view list:%s", list);
    }

    // Histogram line — nonzero bins only.
    {
        char hist[768];
        int n = 0;
        uint64_t nonzero = 0;
        for (uint32_t op = 0; op < OPCODE_COUNT; op++) {
            if (g_cmd_hist[op] > 0 && n < (int)sizeof(hist) - 24) {
                n += _snprintf(hist + n, sizeof(hist) - n, " %02u=%llu", op,
                               (unsigned long long)g_cmd_hist[op]);
                nonzero++;
            }
        }
        hist[n] = '\0';
        MC2VR_LOG("cmds: total=%llu in window (%u opcodes active):%s",
                  (unsigned long long)g_cmd_total, (uint32_t)nonzero,
                  nonzero ? hist : " (stream empty — no world packets?)");
    }

    // Slots + queue line. prodA is a ring position: raw window stats only.
    MC2VR_LOG("slots: endOfFrame=%llu postUpdate=%llu | queue: elem=%u cap=%u "
              "prodA last=%u min=%u max=%u prodB=%u changedPolls=%llu",
              (unsigned long long)g_slot4_calls, (unsigned long long)hooks::post_update_calls(),
              g_queue_elem, g_queue_cap, g_prod_a_last, g_prod_a_min, g_prod_a_max,
              g_prod_b_last, (unsigned long long)g_queue_changed_polls);

    // Reset window aggregates.
    for (uint32_t op = 0; op < OPCODE_COUNT; op++) {
        g_cmd_hist[op] = 0;
    }
    g_cmd_total = 0;
    g_view_submits = 0;
    g_view_frames = 0;
    g_vpf_min = 0;
    g_vpf_max = 0;
    for (uint32_t i = 0; i < g_view_distinct; i++) {
        g_views[i] = {0, 0, 0};
    }
    g_view_distinct = 0;
    g_prod_a_min = 0xffffffff;
    g_prod_a_max = 0;
    g_queue_changed_polls = 0;

    view_rewrite::report_window();
    pacing::report_window();
    wait_census::report_window();
    gpu_census::report_window();
    ps_census::report_window();
    msaa::report_window();
    hud::report_window();
    world_markers::report_window();
    gui_census::report_window();
    camera_table::report_window();
    cull_frustum::report_window();
    occlusion_queries::report_window();
    build_census::report_window();
    s2c::report_window();
    eye_replay::report_window();
    render_outputs::report_window();
    render_size::report_window();
    watch::report_window();
    camera_census::report_window();
    gameplay_census::report_window();
}



DWORD WINAPI poller_thread(LPVOID)
{
    // One-time reads of the constant fields (after carrier init they are set).
    g_queue_elem = *(const uint32_t *)(MC2_G_RENDERQUEUE + 0x4);
    g_queue_cap = *(const uint32_t *)(MC2_G_RENDERQUEUE + 0x8);
    uint32_t polls = 0;
    uint32_t windows = 0;
    uint32_t refresh_rotor = 0;

    while (InterlockedCompareExchange(&g_poller_run, 1, 1)) {
        Sleep(POLL_MS);
        crash_log::watchdog_tick();  // HANG: reports (crash_log.hpp)

        // Culling-watch: one-time DR0-3 arm once a target view exists
        // (per-thread DRs cannot be set from the watching thread itself).
        watch::poll();

        const uint32_t a = *(const uint32_t *)(MC2_G_RENDERQUEUE + 0x10);
        const uint32_t b = *(const uint32_t *)(MC2_G_RENDERQUEUE + 0x14);
        if (a != g_prod_a_last || b != g_prod_b_last) {
            g_queue_changed_polls++;
        }
        g_prod_a_last = a;
        g_prod_b_last = b;
        if (a < g_prod_a_min) {
            g_prod_a_min = a;
        }
        if (a > g_prod_a_max) {
            g_prod_a_max = a;
        }

        if (++polls >= REPORT_POLLS) {
            polls = 0;

            // Pick the refresh target BEFORE report_window() clears the
            // aggregation (reading after it always saw distinct=0 — that
            // ordering bug silently disabled steady-state re-dumps).
            uint32_t distinct = g_view_distinct; // approximate cross-thread read
            uint32_t pick = UINT32_MAX;
            if (distinct > 0) {
                pick = g_views[refresh_rotor++ % distinct].idx;
            }

            report_window();
            windows++;

            if (g_view_dumps && windows % REFRESH_EVERY_WINDOWS == 0 && g_refresh_done < REFRESH_MAX &&
                pick != UINT32_MAX) {
                InterlockedExchange(&g_refresh_target, (LONG)pick);
            }
        }
    }
    return 0;
}

// ---- install ----------------------------------------------------------------

bool install_mid(SafetyHookMid &storage, uintptr_t target, safetyhook::MidHookFn fn,
                 const char *name)
{
    if (!hooks::create_mid(storage, target, fn, name)) {
        return false;
    }
    MC2VR_LOG("installed %s MidHook @ %p", name, (void *)target);
    return true;
}

} // namespace

void on_render_command(const uint32_t *cmd, const uint32_t *stream, uint32_t arg2, uint32_t arg3)
{
    gui_census::on_command(cmd);
    const uint32_t op = cmd[0];
    if (op < OPCODE_COUNT) {
        g_cmd_hist[op]++;
    }
    g_cmd_total++;
    s2c::on_command(cmd, stream, arg2, arg3);
}

bool set_view_dumps(const char *value)
{
    if (strcmp(value, "on") == 0) g_view_dumps = true;
    else if (strcmp(value, "off") == 0) g_view_dumps = false;
    else return false;
    MC2VR_LOG("views: debug_view_dumps=%s", value);
    return true;
}

void install_early()
{
    // View-table dump.
    install_mid(g_view_mid, MC2_SUBMITVIEW_LOOP_LEA, view_midhook, "SubmitWorldPackets view loop");

    // FrameTick's g_RenderShell slot-4 call site: hit counter (the slot-5
    // site is the IPC event drain, hooks/frame_tick.cpp).
    install_mid(g_endofframe_mid, MC2_FRAMETICK_ENDOFFRAME_SITE, endofframe_midhook,
                "FrameTick EndOfFrameHook call site");


    // Optional scene -> draw-record build census (mc2vr.conf debug_build_census=on).
    build_census::install();
    // Optional occlusion culling off (mc2vr.conf debug_occlusion_off=on).
    occlusion_off::install();
    // Optional gameplay census (mc2vr.conf debug_gameplay_census=on).
    gameplay_census::install();
}

void install_late()
{
    // Queue counter poller.
    InterlockedExchange(&g_poller_run, 1);
    g_poller_thread = CreateThread(nullptr, 0, poller_thread, nullptr, 0, nullptr);
    if (!g_poller_thread) {
        MC2VR_LOG("FATAL — queue poller thread creation failed (%lu)", GetLastError());
    }
}

} // namespace mc2vr::render
