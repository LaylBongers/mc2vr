// S1b implementation — see s1_probe.hpp for the design map and the S1 run
// results that shaped it. Per-address facts come from the Ghidra plate
// comments (SubmitWorldPackets S0 analysis); the decision tree that consumes
// the logged evidence is docs/stereo_design.md §S1 + §"S1 run results".
//
// Read-safety discipline: every live-pointer deref here is either bounded to
// a known image range (view-table walk: fixed base, idx < 512, hop cap) or
// structurally validated before use (frame-ctx pointer: its 0x680 block must
// contain &g_RenderQueue at +0x60 and g_ViewTable at +0xC4). Proven-mechanism
// rule unchanged: plaintext reads only, no VM-region access.

#include "s1_probe.hpp"

#include <safetyhook.hpp>

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "game_addresses.h"
#include "hooks.hpp"
#include "log.hpp"

namespace mc2vr::s1 {

namespace {

// ---- static facts (Ghidra /RenderPath plate comments, S0) -----------------

constexpr uintptr_t VIEW_TABLE = MC2_VIEW_TABLE;
constexpr uint32_t VIEW_STRIDE = 0x810;
constexpr uint32_t VIEW_MATRIX_OFF = 0x20;    // nine 4x4 matrices, stride 0x40
constexpr uint32_t VIEW_MATRIX_STRIDE = 0x40;
constexpr uint32_t VIEW_MATRIX_COUNT = 9;
constexpr uint32_t VIEW_M0_OFF = 0x20;        // m[0] viewToWorld (camera pos)
constexpr uint32_t VIEW_M1_OFF = 0x60;        // m[1] worldToView (negated pos)
constexpr uint32_t VIEW_LINK_OFF = 0x04;      // next index; negative terminates
constexpr uint32_t VIEW_LIST_MAX_HOPS = 300;
constexpr uint32_t VIEW_IDX_MAX = 512;        // table is ~256 entries; slack

// Frame-ctx layout (SubmitWorldPackets this+...): 0x680 block at +0xd2950
// (S0), per-view camera staging slots at +0xc2110 (stride 0x30; content per
// walk: pos3 at +0x00, serial +0x0c, rot16 +0x10, lodByte +0x20).
constexpr uint32_t CTX_BLOCK_OFF = 0xd2950;
constexpr uint32_t CTX_BLOCK_SIZE = 0x680;
constexpr uint32_t CTX_QUEUEPTR_OFF = 0x60;  // &g_RenderQueue (also +0x6c/+0x9c/+0xa8)
constexpr uint32_t CTX_TABLEPTR_OFF = 0xC4; // g_ViewTable
constexpr uint32_t CTX_SUBPTR_OFF = 0x74;    // resolved primary-subobject ptrs[2]
constexpr uint32_t CTX_SUBOBJ_SCAN = 0x3a0; // live primary-subobject size
constexpr uint32_t CTX_STAGING_OFF = 0xc2110;
constexpr uint32_t STAGING_STRIDE = 0x30;

constexpr uint32_t D3DTS_VIEW = 2;
constexpr uint32_t D3DTS_PROJECTION = 3;

// ---- logging budget knobs ---------------------------------------------------

constexpr uint32_t FRAME_SAMPLES = 8;      // per-frame bracket ring (frame % 8)
constexpr uint32_t BRACKET_BURST = 3;      // one-shot full bracket lines
constexpr uint32_t RAW_SERIES_MAX = 60;     // one-shot raw-counter lines (world-view frames)
constexpr uint32_t ELEM_DUMP_FRAMES = 3;    // ring-element dumps (frames with world views)
constexpr uint32_t ELEM_DUMP_FORWARD = 12;  // elements dumped ahead of the consumer position
constexpr uint32_t FRAME_LIST_MAX = 64;     // per-frame (idx,type,flags) recs (list line only)
constexpr uint32_t LIST_LOG_ENTRIES = 40;   // entries in a full-list log line
constexpr uint32_t SIG_SLOTS = 6;          // distinct view-list signatures / window
constexpr uint32_t RECT_SLOTS = 16;        // distinct viewport rects / window
constexpr uint32_t VP_BURST = 3;           // one-shot full viewport lines
constexpr uint32_t SATELLITE_MIN_VIEWS = 100;
constexpr uint32_t SATELLITE_LOG_MAX = 2;   // full satellite lists / window
constexpr uint32_t VS_ROWS = 256;           // VS constant float4 cache (vs_2_0 max)
constexpr uint32_t VS_BULK_MAX_GROUPS = 64; // per-call cap on bulk 4-float-aligned classification
constexpr uint32_t VS_DETAIL_BURST = 8;     // one-shot vsmat detail lines

// S1.3 residency patch v2: two spaced windows on a LIVE head view
// (nonzero m[0] translation — the first run patched an all-zero template
// view 0 and proved nothing).
constexpr float PATCH_DELTA = 4.0f;
constexpr uint32_t PATCH_FRAMES = 5;        // frames per window (~0.08s)
constexpr uint32_t PATCH_GAP = 120;         // frames between windows (~2s, so
                                            // the two nudges are separately
                                            // visible)
constexpr uint32_t PATCH_TRY_MAX = 3600;    // ~60s at 60fps to find a live head

// ---- hook storage (leaked by design, same teardown reasoning as M2/M3) ------

SafetyHookMid g_pre_vm_mid;
SafetyHookMid g_renderframe_mid;

// ---- S1.1 consumer bracket (countersA = queue+0x10 is a 32-bit cumulative
// monotonic counter — S1 run result; the S0 packed-u16 decode was wrong) ------

struct FrameSample {
    uint64_t frame = UINT64_MAX;
    uint32_t a_pre = 0;    // countersA at the pre-VM site
    uint32_t a_rf = 0;     // at RenderFrame entry
    uint32_t a_cmd = 0;    // at first stream command
    uint32_t a_eof = 0;    // at EndOfFrameHook
    uint32_t b_pre = 0;    // countersB at the pre-VM site
    uint32_t full_delta = 0; // a_pre(frame) - a_pre(frame-1)
    uint32_t views = 0;      // world views walked this frame (total)
    uint8_t seen = 0;        // bit0 pre, 1 rf, 2 cmd, 3 eof
    bool classified = false;
};
FrameSample g_samples[FRAME_SAMPLES];
uint64_t g_bracket_logged = 0;
bool g_rf_logged = false;
uint32_t g_prev_a_pre = 0;

// Window aggregates (reset in report_window()).
uint64_t g_full_brackets = 0;
uint64_t g_adv_pipeline = 0;
uint64_t g_adv_rf_to_cmd = 0;
uint64_t g_adv_cmd_to_eof = 0;
uint64_t g_adv_none = 0;
uint32_t g_elem_dumps = 0;     // process-lifetime one-shot counter
uint64_t g_raw_series = 0;     // process-lifetime one-shot counter

inline uint32_t read_counters_a()
{
    return *(volatile uint32_t *)MC2_QUEUE_COUNTERS_A;
}

FrameSample &sample_for(uint64_t frame)
{
    FrameSample &s = g_samples[frame % FRAME_SAMPLES];
    if (s.frame != frame) {
        s = FrameSample{};
        s.frame = frame;
    }
    return s;
}

// Decision-tree classification (stereo_design.md): countersA advancing
// pre-VM -> RenderFrame entry = consumer at pipeline time (the only code
// between the two points is the 0x0050f660 call) — the clone-at-stage
// precondition. Additional advance entry -> first stream cmd = consumption
// also happens inside RenderFrame before the command stream runs.
void classify_sample(FrameSample &s)
{
    if (s.classified || (s.seen & 0x3) != 0x3) {
        return; // needs pre + rf at minimum
    }
    s.classified = true;
    g_full_brackets++;

    const uint32_t d_pipe = s.a_rf - s.a_pre; // monotonic 32-bit; no wrap in-run
    const bool has_cmd = (s.seen & 0x4) != 0;
    const bool has_eof = (s.seen & 0x8) != 0;
    const uint32_t d_cmd = has_cmd ? s.a_cmd - s.a_rf : 0;
    const uint32_t d_eof = (has_cmd && has_eof) ? s.a_eof - s.a_cmd : 0;

    if (d_pipe != 0) {
        g_adv_pipeline++;
    }
    if (d_cmd != 0) {
        g_adv_rf_to_cmd++;
    }
    if (d_eof != 0) {
        g_adv_cmd_to_eof++;
    }
    if (d_pipe == 0 && d_cmd == 0 && d_eof == 0) {
        g_adv_none++;
    }

    if (g_bracket_logged < BRACKET_BURST) {
        g_bracket_logged++;
        MC2VR_LOG("S1 bracket: frame=%llu views=%u A pre=%08x rf=%08x cmd=%08x "
                  "eof=%08x B=%08x | adv pre->rf=%u rf->cmd=%u cmd->eof=%u "
                  "fullFrame=%u",
                  (unsigned long long)s.frame, s.views, s.a_pre, s.a_rf, s.a_cmd,
                  s.a_eof, s.b_pre, d_pipe, d_cmd, d_eof, s.full_delta);
    }

    // One-shot raw-counter series over world-view frames: the offline decode
    // basis for the ring position/counter semantics (S1 run result: the S0
    // formula and the packed-u16 decode were both wrong).
    if (s.views > 0 && g_raw_series < RAW_SERIES_MAX) {
        g_raw_series++;
        MC2VR_LOG("S1 raw: frame=%llu views=%u A pre=%08x rf=%08x cmd=%08x "
                  "eof=%08x B=%08x | d(pre->rf)=%u d(rf->cmd)=%u d(cmd->eof)=%u "
                  "fullFrame=%u",
                  (unsigned long long)s.frame, s.views, s.a_pre, s.a_rf, s.a_cmd,
                  s.a_eof, s.b_pre, d_pipe, d_cmd, d_eof, s.full_delta);
    }
}

// ---- S1.1 element layout dump (frames with world views only) ----------------

void dump_ring_elements(uint32_t a, uint32_t b)
{
    const uint32_t elem = *(const uint32_t *)MC2_QUEUE_ELEM_SIZE;
    const uint32_t cap = *(const uint32_t *)MC2_QUEUE_CAPACITY;
    const uint8_t *buf = *(const uint8_t *const *)MC2_QUEUE_BUFFER;
    // +0xc must be a POINTER (the counters sit at +0x10, so the ring cannot
    // be inline); still guard the range before dereferencing it.
    if (!buf || (uintptr_t)buf < 0x00100000 || (uintptr_t)buf >= 0x80000000 ||
        elem == 0 || elem > 256 || cap == 0 || cap > 65536 ||
        (uint64_t)elem * cap > 64ull * 1024 * 1024) {
        MC2VR_LOG("S1 elem: queue fields not sane (elem=%u cap=%u buf=%p) — dump skipped",
                  elem, cap, (const void *)buf);
        return;
    }

    // At the pre-VM site the current frame's staged elements sit AHEAD of the
    // consumer counter (consumption happens after this point — S1 bracket
    // evidence), so dump forward from A % cap.
    const uint32_t end = a % cap;
    MC2VR_LOG("S1 elem: A=%08x B=%08x consPos=%u (dumping %u elems forward; "
              "world elements expect +0x1c/0x24/0x2c = 30/810/680 + live ptrs)",
              a, b, end, ELEM_DUMP_FORWARD);
    for (uint32_t i = 0; i < ELEM_DUMP_FORWARD; i++) {
        const uint8_t *p = buf + (size_t)((end + i) % cap) * elem;
        for (uint32_t off = 0; off < elem; off += 32) {
            const uint32_t n = elem - off < 32 ? elem - off : 32;
            char hex[96];
            char *w = hex;
            for (uint32_t j = 0; j < n; j++) {
                *w++ = "0123456789abcdef"[p[off + j] >> 4];
                *w++ = "0123456789abcdef"[p[off + j] & 0xf];
            }
            *w = '\0';
            MC2VR_LOG("S1 elem @consPos+%u +0x%02x: %s", i, off, hex);
        }
    }
}

// ---- S1.4 per-frame view list (signature-deduped logging) --------------------

struct ViewRec {
    uint16_t idx;
    uint16_t type;
    uint32_t flags;
};
ViewRec g_list[FRAME_LIST_MAX];
uint32_t g_list_n = 0;        // capped record count (log line only)
uint32_t g_list_total = 0;    // uncapped walk count (satellite detection)
uint32_t g_list_t2 = 0;
uint64_t g_list_frame = UINT64_MAX;
int32_t g_list_head = -1;

uintptr_t g_frame_ctx = 0;
bool g_ctx_checked = false;
bool g_ctx_valid = false;

struct Sig {
    uint32_t hash;
    uint64_t frames;
};
Sig g_sigs[SIG_SLOTS];
uint32_t g_sig_n = 0;
uint64_t g_list_first_logged = 0; // process-lifetime full-list logs
uint64_t g_sat_logged_window = 0;

// Window aggregates.
uint64_t g_list_frames = 0;
uint64_t g_sat_frames = 0;
uint32_t g_sat_max = 0;
uint32_t g_t2_min = 0xffffffff, g_t2_max = 0;

bool type_flags_mismatch_logged = false;

// ---- S1.3 residency patch v2 ---------------------------------------------------

enum class PatchState { Idle, WindowA, Gap, WindowB, Done };
PatchState g_patch_state = PatchState::Idle;
const uint8_t *g_patch_entry = nullptr; // window A target (head ViewEntry)
float g_patch_saved[16];
uint8_t *g_patch_slot = nullptr;        // window B target (staging slot pos)
float g_patch_slot_saved = 0.0f;
int32_t g_patch_head = -1;
uint32_t g_patch_left = 0;
uint32_t g_patch_gap_left = 0;
uint32_t g_patch_tries = 0;
bool g_residency_seen = false;
bool g_residency_logged = false;

// ---- S1.2 aggregates (SetTransform: kept to document its deadness;
// VS constants: the real camera channel) -----------------------------------------

uint64_t g_x_calls = 0, g_x_view = 0, g_x_proj = 0, g_x_other = 0;
uint64_t g_x_detail = 0;

uint64_t g_vp_calls = 0;
struct RectRec {
    uint32_t x, y, w, h;
    uint64_t count;
};
RectRec g_rects[RECT_SLOTS];
uint32_t g_rect_n = 0;
uint64_t g_vp_frame = UINT64_MAX;
uint32_t g_vpf_frame_n = 0;
uint32_t g_vpf_min = 0xffffffff, g_vpf_max = 0;
uint64_t g_vp_burst = 0;

// VS constant channel (S1b).
uint64_t g_vs_calls = 0, g_vs_vec4s = 0;
uint64_t g_vs_classified = 0;
uint64_t g_vs_detail = 0;
struct VsMatchAgg {
    uint64_t entry, entryT, ctx, sub, none;
};
VsMatchAgg g_vs{};
uint32_t g_vs_reg_hits[VS_ROWS] = {};
uint64_t g_vs_frame = UINT64_MAX;
float g_vs_rows[VS_ROWS][4] = {};
uint8_t g_vs_valid[VS_ROWS] = {};

// ---- helpers ------------------------------------------------------------------

bool matrix_eq(const float *a, const float *b)
{
    return memcmp(a, b, 64) == 0;
}

bool matrix_eq_T(const float *a, const float *b)
{
    for (uint32_t r = 0; r < 4; r++) {
        for (uint32_t c = 0; c < 4; c++) {
            if (a[r * 4 + c] != b[c * 4 + r]) {
                return false;
            }
        }
    }
    return true;
}

// Scan a memory region (4-byte aligned windows) for the 16-float sequence.
bool find_in_region(const float *m, const uint8_t *base, uint32_t size, uint32_t &off_out)
{
    if (!base) {
        return false;
    }
    for (uint32_t off = 0; off + 64 <= size; off += 4) {
        if (matrix_eq(m, (const float *)(base + off))) {
            off_out = off;
            return true;
        }
    }
    return false;
}

// Walk the active-view list, compare against all nine matrices per entry.
bool find_in_entries(const float *m, int32_t &idx_out, uint32_t &k_out, bool &t_out)
{
    int32_t idx = *(volatile int32_t *)MC2_VIEW_LIST_HEAD;
    for (uint32_t hops = 0; idx >= 0 && (uint32_t)idx < VIEW_IDX_MAX && hops < VIEW_LIST_MAX_HOPS;
         hops++) {
        const uint8_t *entry = (const uint8_t *)(VIEW_TABLE + (size_t)idx * VIEW_STRIDE);
        for (uint32_t k = 0; k < VIEW_MATRIX_COUNT; k++) {
            const float *km = (const float *)(entry + VIEW_MATRIX_OFF + k * VIEW_MATRIX_STRIDE);
            if (matrix_eq(m, km)) {
                idx_out = idx;
                k_out = k;
                t_out = false;
                return true;
            }
            if (matrix_eq_T(m, km)) {
                idx_out = idx;
                k_out = k;
                t_out = true;
                return true;
            }
        }
        idx = *(volatile int32_t *)(entry + VIEW_LINK_OFF);
    }
    return false;
}

// Shared classification core. Sets exactly one class; callers do their own
// counting/tagging/logging.
struct MatrixMatch {
    enum class Kind { None, Entry, Ctx, Sub } kind = Kind::None;
    const uint8_t *entry = nullptr; // Kind::Entry
    int32_t idx = -1;
    uint32_t k = 0;
    bool transposed = false;
    uint32_t region_off = 0; // Kind::Ctx / Kind::Sub
    uint32_t sub_idx = 0;     // Kind::Sub
};

MatrixMatch find_matrix(const float *m)
{
    MatrixMatch mm;
    int32_t idx = -1;
    uint32_t k = 0;
    bool t = false;
    if (find_in_entries(m, idx, k, t)) {
        mm.kind = MatrixMatch::Kind::Entry;
        mm.entry = (const uint8_t *)(VIEW_TABLE + (size_t)idx * VIEW_STRIDE);
        mm.idx = idx;
        mm.k = k;
        mm.transposed = t;
        return mm;
    }
    const uint8_t *block =
        g_ctx_valid ? (const uint8_t *)(g_frame_ctx + CTX_BLOCK_OFF) : nullptr;
    uint32_t off = 0;
    if (find_in_region(m, block, CTX_BLOCK_SIZE, off)) {
        mm.kind = MatrixMatch::Kind::Ctx;
        mm.region_off = off;
        return mm;
    }
    for (uint32_t s = 0; block && s < 2; s++) {
        const uint8_t *sub = *(const uint8_t *const *)(block + CTX_SUBPTR_OFF + s * 4);
        if (find_in_region(m, sub, CTX_SUBOBJ_SCAN, off)) {
            mm.kind = MatrixMatch::Kind::Sub;
            mm.sub_idx = s;
            mm.region_off = off;
            return mm;
        }
    }
    return mm;
}

void format_match_tag(const MatrixMatch &mm, char *out, size_t out_size)
{
    switch (mm.kind) {
    case MatrixMatch::Kind::Entry:
        _snprintf(out, out_size, "e%d.m%u%s", mm.idx, mm.k, mm.transposed ? "T" : "");
        break;
    case MatrixMatch::Kind::Ctx:
        _snprintf(out, out_size, "ctx+0x%03x", mm.region_off);
        break;
    case MatrixMatch::Kind::Sub:
        _snprintf(out, out_size, "sub%u+0x%03x", mm.sub_idx, mm.region_off);
        break;
    default:
        strncpy(out, "none", out_size - 1);
        out[out_size - 1] = '\0';
        break;
    }
    if (out_size) {
        out[out_size - 1] = '\0';
    }
}

// Residency proof (window A): a matrix equal to the live PATCHED entry m[1]
// reaching the GPU means the consumer derefs the live ViewEntry at consume
// time — the clone-at-stage design rests on this.
void check_residency(const MatrixMatch &mm, uint64_t frame)
{
    if (mm.kind == MatrixMatch::Kind::Entry && !mm.transposed && mm.k == 1 &&
        mm.entry == g_patch_entry && g_patch_state == PatchState::WindowA) {
        g_residency_seen = true;
        if (!g_residency_logged) {
            g_residency_logged = true;
            MC2VR_LOG("S1 RESIDENCY PROVEN: GPU-bound matrix matches the live "
                      "PATCHED ViewEntry m[1] (delta %g) at frame=%llu — consumer "
                      "derefs the live entry at consume time",
                      (double)PATCH_DELTA, (unsigned long long)frame);
        }
    }
}

void hex64(const float *m, char *out, size_t out_size)
{
    const uint8_t *raw = (const uint8_t *)m;
    char *w = out;
    for (uint32_t i = 0; i < 64 && (size_t)(w - out) + 2 < out_size; i++) {
        *w++ = "0123456789abcdef"[raw[i] >> 4];
        *w++ = "0123456789abcdef"[raw[i] & 0xf];
    }
    *w = '\0';
}

// ---- S1.4 helpers ---------------------------------------------------------------

void log_view_list(uint64_t frame)
{
    if (g_list_frame != frame || g_list_total == 0) {
        return; // no world views walked this frame (menu etc.)
    }
    g_list_frames++;

    // Signature over the records in walk order (order = activation recency,
    // head + totals included — a change in any yields a new signature).
    uint32_t h = 0x811c9dc5;
    auto mix = [&h](uint32_t v) {
        h = (h ^ v) * 0x01000193;
    };
    mix((uint32_t)g_list_head);
    mix(g_list_n);
    mix(g_list_total);
    mix(g_list_t2);
    for (uint32_t i = 0; i < g_list_n; i++) {
        mix(g_list[i].idx);
        mix(g_list[i].type);
        mix(g_list[i].flags);
    }

    Sig *slot = nullptr;
    bool fresh = false;
    for (uint32_t i = 0; i < g_sig_n; i++) {
        if (g_sigs[i].hash == h) {
            slot = &g_sigs[i];
            break;
        }
    }
    if (!slot && g_sig_n < SIG_SLOTS) {
        slot = &g_sigs[g_sig_n++];
        slot->hash = h;
        slot->frames = 0;
        fresh = true;
    }
    if (slot) {
        slot->frames++;
    }

    // Satellite detection on the UNCAPPED total (S1 bug: the capped record
    // count could never exceed FRAME_LIST_MAX).
    const bool satellite = g_list_total > SATELLITE_MIN_VIEWS;
    if (satellite) {
        g_sat_frames++;
        if (g_list_total > g_sat_max) {
            g_sat_max = g_list_total;
        }
    }
    if (g_list_t2 < g_t2_min) {
        g_t2_min = g_list_t2;
    }
    if (g_list_t2 > g_t2_max) {
        g_t2_max = g_list_t2;
    }

    // Full-list logs: the first two frames ever, the first frame of each new
    // signature (<= SIG_SLOTS/window), and satellite frames (capped) — the
    // steady state must not produce a line per frame.
    bool do_log = g_list_first_logged < 2 || fresh;
    if (satellite && g_sat_logged_window < SATELLITE_LOG_MAX) {
        g_sat_logged_window++;
        do_log = true;
    }
    if (!do_log) {
        return;
    }
    g_list_first_logged++;

    char list[880];
    int n = _snprintf(list, sizeof(list), "head=%d n=%u t2=%u sig=%08x ctx=%p:",
                      g_list_head, g_list_total, g_list_t2, h, (void *)g_frame_ctx);
    const uint32_t shown = g_list_n < LIST_LOG_ENTRIES ? g_list_n : LIST_LOG_ENTRIES;
    for (uint32_t i = 0; i < shown && n < (int)sizeof(list) - 24; i++) {
        n += _snprintf(list + n, sizeof(list) - n, " i%u/t%u/f%04x", g_list[i].idx,
                       g_list[i].type, g_list[i].flags >> 16);
    }
    if (g_list_total > shown && n < (int)sizeof(list) - 24) {
        _snprintf(list + n, sizeof(list) - n, " ...(%u more)", g_list_total - shown);
    }
    list[sizeof(list) - 1] = '\0';
    MC2VR_LOG("S1 vlist: frame=%llu %s", (unsigned long long)frame, list);
}

// ---- S1.3 residency patch v2 driver ---------------------------------------------

bool head_is_live(const uint8_t *&entry_out, int32_t &head_out)
{
    const int32_t head = *(volatile int32_t *)MC2_VIEW_LIST_HEAD;
    if (head < 0 || (uint32_t)head >= VIEW_IDX_MAX) {
        return false;
    }
    const uint8_t *entry = (const uint8_t *)(VIEW_TABLE + (size_t)head * VIEW_STRIDE);
    const uintptr_t ref = *(const uintptr_t *)(entry + MC2_VIEW_OBJ_PTR_OFF);
    if (!ref) {
        return false;
    }
    if ((*(const uint32_t *)(ref + MC2_VIEW_REF_TYPEFLAGS_OFF) & 0xffff) != 2) {
        return false; // not a world view
    }
    // Liveness (S1 lesson: head view 0 is a type-2 ALL-ZERO template): the
    // viewToWorld matrix must carry a camera translation.
    const float *m0 = (const float *)(entry + VIEW_M0_OFF);
    if (m0[12] == 0.0f && m0[13] == 0.0f && m0[14] == 0.0f) {
        return false;
    }
    entry_out = entry;
    head_out = head;
    return true;
}

void patch_apply_m1(uint64_t frame)
{
    memcpy(g_patch_saved, g_patch_entry + VIEW_M1_OFF, sizeof(g_patch_saved));
    ((float *)(g_patch_entry + VIEW_M1_OFF))[12] += PATCH_DELTA;
    (void)frame;
}

void patch_apply_slot(uint64_t frame)
{
    g_patch_slot_saved = *(const float *)g_patch_slot;
    *(float *)g_patch_slot += PATCH_DELTA;
    (void)frame;
}

void run_patch(uint64_t frame)
{
    switch (g_patch_state) {
    case PatchState::Done:
        return;

    case PatchState::WindowA:
        // Restore the previous frame's m[1]: the game may rewrite m[1] at any
        // point, so never keep a patch past its frame.
        memcpy((void *)(g_patch_entry + VIEW_M1_OFF), g_patch_saved, sizeof(g_patch_saved));
        if (--g_patch_left == 0) {
            MC2VR_LOG("S1 patch A (m1) ended: head=%d — residency_seen=%u "
                      "(nudge observed? consumer reads m[1] live; none? m[1] is "
                      "not the draw-camera source)",
                      g_patch_head, g_residency_seen ? 1u : 0u);
            g_patch_state = PatchState::Gap;
            g_patch_gap_left = PATCH_GAP;
            return;
        }
        patch_apply_m1(frame); // re-apply relative to current values
        return;

    case PatchState::Gap:
        if (--g_patch_gap_left > 0) {
            return;
        }
        // Fall through to WindowB setup.
        g_patch_state = PatchState::WindowB;
        g_patch_left = 0;
        [[fallthrough]];

    case PatchState::WindowB: {
        if (g_patch_slot) {
            *(float *)g_patch_slot = g_patch_slot_saved; // restore previous frame
            const int32_t head = *(volatile int32_t *)MC2_VIEW_LIST_HEAD;
            if (head != g_patch_head) {
                MC2VR_LOG("S1 patch B (staging slot) aborted: head changed %d -> %d",
                          g_patch_head, head);
                g_patch_slot = nullptr;
                g_patch_state = PatchState::Done;
                return;
            }
            if (--g_patch_left == 0) {
                MC2VR_LOG("S1 patch B (staging slot) ended: head=%d slot=%p — "
                          "(nudge observed? draw camera reads the staging slot at "
                          "consume time)",
                          g_patch_head, (const void *)g_patch_slot);
                g_patch_slot = nullptr;
                g_patch_state = PatchState::Done;
                return;
            }
            patch_apply_slot(frame);
            return;
        }
        // Window B not started yet: need a live head (same one) + a validated ctx.
        const uint8_t *entry = nullptr;
        int32_t head = -1;
        if (!g_ctx_valid || !head_is_live(entry, head)) {
            if (++g_patch_tries > PATCH_TRY_MAX) {
                g_patch_state = PatchState::Done;
                MC2VR_LOG("S1 patch B: no live head/ctx in %u frames — skipped",
                          (unsigned)PATCH_TRY_MAX);
            }
            return;
        }
        g_patch_head = head;
        g_patch_slot = (uint8_t *)(g_frame_ctx + CTX_STAGING_OFF + (size_t)head * STAGING_STRIDE);
        g_patch_left = PATCH_FRAMES;
        patch_apply_slot(frame);
        MC2VR_LOG("S1 patch B (staging slot): head=%d slot=%p pos[0] += %g for %u "
                  "frames — expected visual: second brief nudge ~%u frames after "
                  "the first",
                  head, (const void *)g_patch_slot, (double)PATCH_DELTA,
                  (unsigned)PATCH_FRAMES, (unsigned)PATCH_GAP);
        return;
    }

    case PatchState::Idle: {
        if (++g_patch_tries > PATCH_TRY_MAX) {
            g_patch_state = PatchState::Done;
            MC2VR_LOG("S1 patch A: no live head view in %u frames — proof skipped",
                      (unsigned)PATCH_TRY_MAX);
            return;
        }
        const uint8_t *entry = nullptr;
        int32_t head = -1;
        if (!head_is_live(entry, head)) {
            return; // try again next frame
        }
        g_patch_entry = entry;
        g_patch_head = head;
        patch_apply_m1(frame);
        g_patch_state = PatchState::WindowA;
        g_patch_left = PATCH_FRAMES;
        const float *m0 = (const float *)(entry + VIEW_M0_OFF);
        MC2VR_LOG("S1 patch A (m1): head=%d entry=%p (ViewRef=%p) camera pos "
                  "(%g, %g, %g) m1[3] += %g for %u frames — expected visual: "
                  "brief nudge; proof: 'S1 RESIDENCY' or e%d.m1 in S1 vs lines",
                  head, (const void *)entry,
                  (const void *)*(const uintptr_t *)(entry + MC2_VIEW_OBJ_PTR_OFF),
                  (double)m0[12], (double)m0[13], (double)m0[14], (double)PATCH_DELTA,
                  (unsigned)PATCH_FRAMES, head);
        return;
    }
    }
}

// ---- VS constant classification (S1b camera attribution) ------------------------

void vs_classify(const float *m16, uint32_t reg_base, uint64_t frame)
{
    g_vs_classified++;
    const MatrixMatch mm = find_matrix(m16);
    switch (mm.kind) {
    case MatrixMatch::Kind::Entry:
        if (mm.transposed) {
            g_vs.entryT++;
        } else {
            g_vs.entry++;
        }
        break;
    case MatrixMatch::Kind::Ctx:
        g_vs.ctx++;
        break;
    case MatrixMatch::Kind::Sub:
        g_vs.sub++;
        break;
    default:
        g_vs.none++;
        break;
    }
    if (mm.kind != MatrixMatch::Kind::None && reg_base < VS_ROWS) {
        g_vs_reg_hits[reg_base]++;
    }
    check_residency(mm, frame);

    if (g_vs_detail < VS_DETAIL_BURST) {
        g_vs_detail++;
        char tag[48];
        char hex[132];
        format_match_tag(mm, tag, sizeof(tag));
        hex64(m16, hex, sizeof(hex));
        MC2VR_LOG("S1 vsmat: frame=%llu reg=c%u tag=%s m=%s",
                  (unsigned long long)frame, reg_base, tag, hex);
    }
}

// ---- MidHook handlers ------------------------------------------------------------

// 0x004c99f9 — the `call 0x0050f660` itself: handler runs immediately before
// the VM'd packet interpreter, with the whole SubmitWorldPackets walk done.
void pre_vm_midhook(safetyhook::Context &)
{
    const uint64_t frame = hooks::frame_count();
    const uint32_t ca = read_counters_a();
    const uint32_t cb = *(volatile uint32_t *)MC2_QUEUE_COUNTERS_B;

    // The previous frame's four bracket points have all fired by now.
    if (frame > 0) {
        classify_sample(g_samples[(frame - 1) % FRAME_SAMPLES]);
    }

    FrameSample &s = sample_for(frame);
    const uint32_t full_delta = ca - g_prev_a_pre;
    g_prev_a_pre = ca;
    s.a_pre = ca;
    s.b_pre = cb;
    s.full_delta = full_delta;
    s.views = (g_list_frame == frame) ? g_list_total : 0;
    s.seen |= 1;

    // Element dumps re-armed on world-view frames only (S1 lesson: the
    // boot-time dumps only ever captured other producers' elements).
    if (g_elem_dumps < ELEM_DUMP_FRAMES && s.views > 0) {
        g_elem_dumps++;
        dump_ring_elements(ca, cb);
    }

    log_view_list(frame); // S1.4: the walk just ended, the list is final
    run_patch(frame);     // S1.3: patch state resolved before the VM call runs
}

// 0x00855690 — RenderShell_RenderFrame first instruction (consumer half entry).
void renderframe_midhook(safetyhook::Context &ctx)
{
    const uint64_t frame = hooks::frame_count();
    FrameSample &s = sample_for(frame);
    s.a_rf = read_counters_a();
    s.seen |= 2;
    if (!g_rf_logged) {
        g_rf_logged = true;
        MC2VR_LOG("S1: RenderFrame entry first hit: frame=%llu ECX(this?)=%p",
                  (unsigned long long)frame, (const void *)ctx.ecx);
    }
}

} // namespace

// ---- public taps (called from render_dump.cpp / device.cpp handlers) -----------

void note_view(uint32_t idx, uint32_t type, uint32_t flags, uintptr_t frame_ctx)
{
    const uint64_t frame = hooks::frame_count();

    if (!g_ctx_checked && frame_ctx != 0) {
        g_ctx_checked = true;
        g_frame_ctx = frame_ctx;
        // Structural validation before any use (read-safety discipline): the
        // 0x680 block must carry the S0 anchors &g_RenderQueue / g_ViewTable.
        const uint8_t *block = (const uint8_t *)(frame_ctx + CTX_BLOCK_OFF);
        g_ctx_valid = *(const uint32_t *)(block + CTX_QUEUEPTR_OFF) ==
                          (uint32_t)MC2_G_RENDERQUEUE &&
                      *(const uint32_t *)(block + CTX_TABLEPTR_OFF) ==
                          (uint32_t)MC2_VIEW_TABLE;
        MC2VR_LOG("S1: frame-ctx captured EBX=%p block=%p validation=%s "
                  "(expect &queue=%08x at +0x60, table=%08x at +0xC4)",
                  (void *)frame_ctx, (const void *)block, g_ctx_valid ? "OK" : "FAILED",
                  (unsigned)MC2_G_RENDERQUEUE, (unsigned)MC2_VIEW_TABLE);
        if (!g_ctx_valid) {
            g_frame_ctx = 0; // never scan through an unvalidated pointer
        }
    }

    if (frame != g_list_frame) {
        g_list_frame = frame;
        g_list_n = 0;
        g_list_total = 0;
        g_list_t2 = 0;
        g_list_head = *(volatile int32_t *)MC2_VIEW_LIST_HEAD;
    }
    if (g_list_n < FRAME_LIST_MAX) {
        g_list[g_list_n++] = {(uint16_t)idx, (uint16_t)type, flags};
    }
    g_list_total++;
    if (type == 2) {
        g_list_t2++;
    }

    // Consistency check of the flags decode: the type the loop checked
    // (ECX) must equal the low word of the ViewRef dword we read here.
    if (!type_flags_mismatch_logged && (flags & 0xffff) != (type & 0xffff)) {
        type_flags_mismatch_logged = true;
        MC2VR_LOG("S1: WARNING type/flags mismatch idx=%u type=%u flags=%08x — "
                  "ViewRef+0x14 decode wrong, flags data unreliable",
                  idx, type, flags);
    }
}

void note_stream_opcode()
{
    FrameSample &s = sample_for(hooks::frame_count());
    if (s.seen & 0x4) {
        return; // once per frame
    }
    s.seen |= 4;
    s.a_cmd = read_counters_a();
}

void note_end_of_frame()
{
    FrameSample &s = sample_for(hooks::frame_count());
    s.seen |= 8;
    s.a_eof = read_counters_a();
}

void on_set_transform(uint32_t state, const float *m)
{
    // S1 result: the engine never calls this (shader-driven). Keep the
    // counter to re-verify per run; classification stays for completeness.
    g_x_calls++;
    if (!m) {
        g_x_other++;
        return;
    }
    if (state != D3DTS_VIEW && state != D3DTS_PROJECTION) {
        g_x_other++;
        return;
    }
    if (state == D3DTS_VIEW) {
        g_x_view++;
    } else {
        g_x_proj++;
    }
    if (g_x_detail < 3) {
        g_x_detail++;
        char tag[48];
        char hex[132];
        const MatrixMatch mm = find_matrix(m);
        format_match_tag(mm, tag, sizeof(tag));
        hex64(m, hex, sizeof(hex));
        MC2VR_LOG("S1 xform: frame=%llu state=%s tag=%s m=%s",
                  (unsigned long long)hooks::frame_count(),
                  state == D3DTS_VIEW ? "view" : "proj", tag, hex);
    }
}

void on_set_vs_constant(uint32_t start_register, const float *data, uint32_t vec4_count)
{
    g_vs_calls++;
    if (!data || vec4_count == 0) {
        return;
    }
    g_vs_vec4s += vec4_count;

    const uint64_t frame = hooks::frame_count();
    if (frame != g_vs_frame) {
        g_vs_frame = frame;
        memset(g_vs_valid, 0, sizeof(g_vs_valid));
    }

    if (g_vs_calls <= 3) {
        MC2VR_LOG("S1 vs: first call #%llu: start=c%u count=%u frame=%llu",
                  (unsigned long long)g_vs_calls, start_register, vec4_count,
                  (unsigned long long)frame);
    }

    // Bulk path: 4-float-aligned groups inside one call (count >= 4 covers
    // the usual whole-matrix upload).
    if (vec4_count >= 4) {
        const uint32_t groups = vec4_count - 3 < VS_BULK_MAX_GROUPS
                                    ? vec4_count - 3
                                    : VS_BULK_MAX_GROUPS;
        for (uint32_t i = 0; i < groups; i++) {
            vs_classify(data + i * 4, start_register + i, frame);
        }
    }

    // Row-cache path: matrices uploaded one float4 at a time (row r lands in
    // register c_base+r). Cache rows; when c_base..c_base+3 are all present,
    // assemble and classify. Covers both row-major and column-major uploads
    // (the transposed compare catches the latter).
    if (start_register < VS_ROWS) {
        const uint32_t n = vec4_count < VS_ROWS - start_register
                               ? vec4_count
                               : VS_ROWS - start_register;
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t r = start_register + i;
            memcpy(g_vs_rows[r], data + i * 4, 16);
            g_vs_valid[r] = 1;
            if (r >= 3 && g_vs_valid[r - 3] && g_vs_valid[r - 2] && g_vs_valid[r - 1]) {
                float m16[16];
                for (uint32_t row = 0; row < 4; row++) {
                    memcpy(m16 + row * 4, g_vs_rows[r - 3 + row], 16);
                }
                vs_classify(m16, r - 3, frame);
            }
        }
    }
}

void on_set_viewport(uint32_t x, uint32_t y, uint32_t w, uint32_t h, float minz, float maxz)
{
    g_vp_calls++;

    const uint64_t frame = hooks::frame_count();
    if (frame != g_vp_frame) {
        if (g_vp_frame != UINT64_MAX) {
            if (g_vpf_frame_n < g_vpf_min) {
                g_vpf_min = g_vpf_frame_n;
            }
            if (g_vpf_frame_n > g_vpf_max) {
                g_vpf_max = g_vpf_frame_n;
            }
        }
        g_vp_frame = frame;
        g_vpf_frame_n = 0;
    }
    g_vpf_frame_n++;

    for (uint32_t i = 0; i < g_rect_n; i++) {
        if (g_rects[i].x == x && g_rects[i].y == y && g_rects[i].w == w && g_rects[i].h == h) {
            g_rects[i].count++;
            return;
        }
    }
    if (g_rect_n < RECT_SLOTS) {
        g_rects[g_rect_n++] = {x, y, w, h, 1};
    }

    if (g_vp_burst < VP_BURST) {
        g_vp_burst++;
        MC2VR_LOG("S1 vp: frame=%llu rect=%ux%u+%ux%u minZ=%g maxZ=%g",
                  (unsigned long long)frame, x, y, w, h, (double)minz, (double)maxz);
    }
}

void report_window()
{
    MC2VR_LOG("S1 bracket: frames=%llu pipeline=%llu rfToCmd=%llu cmdToEof=%llu "
              "none=%llu | A(lastPre)=%08x",
              (unsigned long long)g_full_brackets, (unsigned long long)g_adv_pipeline,
              (unsigned long long)g_adv_rf_to_cmd, (unsigned long long)g_adv_cmd_to_eof,
              (unsigned long long)g_adv_none, g_prev_a_pre);

    MC2VR_LOG("S1 vs: calls=%llu vec4s=%llu classified=%llu | entry=%llu entryT=%llu "
              "ctx=%llu sub=%llu none=%llu | residency=%s",
              (unsigned long long)g_vs_calls, (unsigned long long)g_vs_vec4s,
              (unsigned long long)g_vs_classified, (unsigned long long)g_vs.entry,
              (unsigned long long)g_vs.entryT, (unsigned long long)g_vs.ctx,
              (unsigned long long)g_vs.sub, (unsigned long long)g_vs.none,
              g_residency_seen ? "PROVEN" : "not-yet");
    {
        // Top matching VS registers (bounded line, insertion sort by hits).
        uint32_t top[6] = {VS_ROWS, VS_ROWS, VS_ROWS, VS_ROWS, VS_ROWS, VS_ROWS};
        for (uint32_t r = 0; r < VS_ROWS; r++) {
            if (g_vs_reg_hits[r] == 0) {
                continue;
            }
            for (int slot = 0; slot < 6; slot++) {
                bool already = false;
                for (int j = 0; j < 6; j++) {
                    already = already || (top[j] == r);
                }
                if (already) {
                    break;
                }
                if (top[slot] == VS_ROWS || g_vs_reg_hits[r] > g_vs_reg_hits[top[slot]]) {
                    for (int j = 5; j > slot; j--) {
                        top[j] = top[j - 1];
                    }
                    top[slot] = r;
                    break;
                }
            }
        }
        char regs[256];
        int n = 0;
        for (int slot = 0; slot < 6 && top[slot] != VS_ROWS && n < (int)sizeof(regs) - 32;
             slot++) {
            n += _snprintf(regs + n, sizeof(regs) - n, " c%u=x%llu", top[slot],
                           (unsigned long long)g_vs_reg_hits[top[slot]]);
        }
        regs[n] = '\0';
        MC2VR_LOG("S1 vsreg: top matched registers:%s", n ? regs : " (none)");
    }

    MC2VR_LOG("S1 xform(SetTransform, expected dead): calls=%llu view=%llu proj=%llu "
              "other=%llu",
              (unsigned long long)g_x_calls, (unsigned long long)g_x_view,
              (unsigned long long)g_x_proj, (unsigned long long)g_x_other);

    {
        char rects[640];
        int n = 0;
        for (uint32_t i = 0; i < g_rect_n && n < (int)sizeof(rects) - 32; i++) {
            n += _snprintf(rects + n, sizeof(rects) - n, " %ux%u@%u,%u x%llu", g_rects[i].w,
                           g_rects[i].h, g_rects[i].x, g_rects[i].y,
                           (unsigned long long)g_rects[i].count);
        }
        rects[n] = '\0';
        MC2VR_LOG("S1 vp: calls=%llu sets/frame min=%u max=%u rects(%u)=%s",
                  (unsigned long long)g_vp_calls, g_vpf_min == 0xffffffff ? 0 : g_vpf_min,
                  g_vpf_max, g_rect_n, n ? rects : " (none)");
    }

    MC2VR_LOG("S1 vlist: frames=%llu t2/frame min=%u max=%u satelliteFrames=%llu "
              "satMax=%u sigs=%u (full lists logged on first sighting)",
              (unsigned long long)g_list_frames, g_t2_min == 0xffffffff ? 0 : g_t2_min,
              g_t2_max, (unsigned long long)g_sat_frames, g_sat_max, g_sig_n);

    // Reset window aggregates. Process-lifetime one-shots (g_bracket_logged,
    // g_raw_series, g_elem_dumps, g_vs_detail, g_vp_burst, g_list_first_logged,
    // g_residency_seen/logged, g_patch_*) are NOT reset.
    g_full_brackets = 0;
    g_adv_pipeline = 0;
    g_adv_rf_to_cmd = 0;
    g_adv_cmd_to_eof = 0;
    g_adv_none = 0;
    g_x_calls = 0;
    g_x_view = 0;
    g_x_proj = 0;
    g_x_other = 0;
    g_vs_calls = 0;
    g_vs_vec4s = 0;
    g_vs_classified = 0;
    g_vs = VsMatchAgg{};
    memset(g_vs_reg_hits, 0, sizeof(g_vs_reg_hits));
    g_vp_calls = 0;
    g_rect_n = 0;
    g_vpf_min = 0xffffffff;
    g_vpf_max = 0;
    g_sig_n = 0;
    g_list_frames = 0;
    g_sat_frames = 0;
    g_sat_max = 0;
    g_sat_logged_window = 0;
    g_t2_min = 0xffffffff;
    g_t2_max = 0;
}

// ---- install --------------------------------------------------------------------

void install()
{
    // Pre-VM consumer bracket (the call instruction itself — SafetyHook
    // relocates the rel32 call into the trampoline; the VM stub is never
    // touched). Best-effort, same policy as the M3 installs.
    auto pre = SafetyHookMid::create(reinterpret_cast<uint8_t *>(MC2_PIPELINE_VMSTUB_CALL),
                                     pre_vm_midhook);
    if (!pre) {
        MC2VR_LOG("S1: FATAL — pre-VM MidHook install failed @ %p (error %u) — "
                  "consumer bracket + patch lost",
                  (void *)MC2_PIPELINE_VMSTUB_CALL, (unsigned)pre.error().type);
    } else {
        g_pre_vm_mid = std::move(*pre);
        MC2VR_LOG("S1: installed pre-VM MidHook @ %p (pipeline call of VM stub 0x0050f660)",
                  (void *)MC2_PIPELINE_VMSTUB_CALL);
    }

    auto rf = SafetyHookMid::create(reinterpret_cast<uint8_t *>(MC2_RENDERSHELL_RENDERFRAME),
                                    renderframe_midhook);
    if (!rf) {
        MC2VR_LOG("S1: FATAL — RenderFrame-entry MidHook install failed @ %p (error %u)",
                  (void *)MC2_RENDERSHELL_RENDERFRAME, (unsigned)rf.error().type);
    } else {
        g_renderframe_mid = std::move(*rf);
        MC2VR_LOG("S1: installed RenderFrame-entry MidHook @ %p",
                  (void *)MC2_RENDERSHELL_RENDERFRAME);
    }
}

} // namespace mc2vr::s1
