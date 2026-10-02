// S1 implementation — see s1_probe.hpp for the design map. Per-address facts
// come from the Ghidra plate comments (SubmitWorldPackets S0 analysis); the
// decision tree that consumes the logged evidence is docs/stereo_design.md
// §S1 "Operational notes".
//
// Read-safety discipline: every live-pointer deref here is either bounded
// to a known image range (view-table walk: fixed base, idx < 512, hop cap)
// or structurally validated before use (frame-ctx pointer: its 0x680 block
// must contain &g_RenderQueue at +0x60 and g_ViewTable at +0xC4 — the same
// anchors the S0 static analysis found). Proven-mechanism rule unchanged:
// plaintext reads only, no VM-region access.

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
constexpr uint32_t VIEW_M1_OFF = 0x60;         // m[1] worldToView (negated pos)
constexpr uint32_t VIEW_LINK_OFF = 0x04;      // next index; negative terminates
constexpr uint32_t VIEW_LIST_MAX_HOPS = 300;
constexpr uint32_t VIEW_IDX_MAX = 512;        // table is ~256 entries; slack

// Frame-ctx 0x680 block (SubmitWorldPackets this+0xd2950).
constexpr uint32_t CTX_BLOCK_OFF = 0xd2950;
constexpr uint32_t CTX_BLOCK_SIZE = 0x680;
constexpr uint32_t CTX_QUEUEPTR_OFF = 0x60;  // &g_RenderQueue (also +0x6c/+0x9c/+0xa8)
constexpr uint32_t CTX_TABLEPTR_OFF = 0xC4; // g_ViewTable
constexpr uint32_t CTX_SUBPTR_OFF = 0x74;    // resolved primary-subobject ptrs[2]
constexpr uint32_t CTX_SUBOBJ_SCAN = 0x3a0; // live primary-subobject size

constexpr uint32_t D3DTS_VIEW = 2;
constexpr uint32_t D3DTS_PROJECTION = 3;

// ---- logging budget knobs ---------------------------------------------------

constexpr uint32_t FRAME_SAMPLES = 8;      // per-frame bracket ring (frame % 8)
constexpr uint32_t BRACKET_BURST = 3;      // one-shot full bracket lines
constexpr uint32_t ELEM_DUMP_FRAMES = 3;   // ring-element dumps, process lifetime
constexpr uint32_t FRAME_LIST_MAX = 64;    // per-frame (idx,type,flags) recs
constexpr uint32_t LIST_LOG_ENTRIES = 40;   // entries in a full-list log line
constexpr uint32_t SIG_SLOTS = 6;          // distinct view-list signatures / window
constexpr uint32_t RECT_SLOTS = 16;        // distinct viewport rects / window
constexpr uint32_t VP_BURST = 3;           // one-shot full viewport lines
constexpr uint32_t XFORM_DETAIL_BURST = 6; // one-shot xform matrix hex lines
constexpr uint32_t SATELLITE_MIN_VIEWS = 100;
constexpr uint32_t SATELLITE_LOG_MAX = 2;  // full satellite lists / window

// S1.3 residency patch: m[1][3] (translation x) of the head view.
constexpr float PATCH_DELTA = 4.0f;
constexpr uint32_t PATCH_FRAMES = 5;        // ~5 frames nudged: enough to survive
                                           // one odd frame, short exposure of
                                           // the patched field to game logic
constexpr uint32_t PATCH_TRY_MAX = 3600;    // ~60s at 60fps to find a type-2 head

// ---- hook storage (leaked by design, same teardown reasoning as M2/M3) ------

SafetyHookMid g_pre_vm_mid;
SafetyHookMid g_renderframe_mid;

// ---- S1.1 consumer bracket ---------------------------------------------------

struct FrameSample {
    uint64_t frame = UINT64_MAX;
    uint32_t cons_pre = 0, prod_pre = 0;  // countersA split at the pre-VM site
    uint32_t cons_rf = 0;                 // countersA.low at RenderFrame entry
    uint32_t cons_cmd = 0;                // ... at first stream command
    uint32_t cons_eof = 0;                // ... at EndOfFrameHook
    uint8_t seen = 0;                      // bit0 pre, 1 rf, 2 cmd, 3 eof
    bool classified = false;
};
FrameSample g_samples[FRAME_SAMPLES];
uint64_t g_bracket_logged = 0;
bool g_rf_logged = false;

// Window aggregates (reset in report_window()).
uint64_t g_full_brackets = 0;
uint64_t g_adv_pipeline = 0;
uint64_t g_adv_rf_to_cmd = 0;
uint64_t g_adv_cmd_to_eof = 0;
uint64_t g_adv_none = 0;
uint32_t g_unc_min = 0xffffffff, g_unc_max = 0;
uint32_t g_elem_dumps = 0; // process-lifetime one-shot counter

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

// Decision-tree classification (stereo_design.md): countersA.low (consumer)
// advancing pre-VM -> RenderFrame entry = interpreter at pipeline time;
// advancing entry -> first stream cmd = consumption inside RenderFrame's
// record walk; advancing after the stream = a later consumer.
void classify_sample(FrameSample &s)
{
    if (s.classified || (s.seen & 0x3) != 0x3) {
        return; // needs pre + rf at minimum
    }
    s.classified = true;
    g_full_brackets++;

    const uint32_t d_pipe = (s.cons_rf - s.cons_pre) & 0xffff;
    const bool has_cmd = (s.seen & 0x4) != 0;
    const bool has_eof = (s.seen & 0x8) != 0;
    const uint32_t d_cmd = has_cmd ? (s.cons_cmd - s.cons_rf) & 0xffff : 0;
    const uint32_t d_eof = (has_cmd && has_eof) ? (s.cons_eof - s.cons_cmd) & 0xffff : 0;

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
        MC2VR_LOG("S1 bracket: frame=%llu cons pre=%u rf=%u cmd=%u eof=%u prod=%u "
                  "| adv pre->rf=%u rf->cmd=%u cmd->eof=%u | rawCountersA=%04x:%04x",
                  (unsigned long long)s.frame, s.cons_pre, s.cons_rf, s.cons_cmd,
                  s.cons_eof, s.prod_pre, d_pipe, d_cmd, d_eof, s.cons_pre, s.prod_pre);
    }
}

// ---- S1.1 element layout dump (one-shot, first few busy frames) --------------

void dump_ring_elements(uint32_t ca, uint32_t cb)
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

    const uint32_t cons = ca & 0xffff;
    const uint32_t prod = ca >> 16;
    const uint32_t unconsumed = (prod - cons) & 0xffff;
    // S0-decoded positions (stereo_design.md) + the two naive candidates, so
    // any counter mis-decode is visible offline against the pair layout.
    const uint32_t doc_cons_pos = (cons + prod) % cap;
    const uint32_t doc_prod_pos = ((cb & 0xffff) + cons) % cap;
    MC2VR_LOG("S1 elem: ca=%04x:%04x cb=%04x cons=%u prod=%u unconsumed=%u "
              "docConsPos=%u docProdPos=%u naiveCons=%u naiveProd=%u",
              cons, prod, cb & 0xffff, cons, prod, unconsumed, doc_cons_pos,
              doc_prod_pos, cons % cap, prod % cap);

    const uint8_t *p = buf + (size_t)doc_cons_pos * elem;
    MC2VR_LOG("S1 elem @docConsPos (3 elems, expect +0x1c/0x24/0x2c = "
              "30/810/680 + live ptrs):");
    for (uint32_t i = 0; i < 3; i++) {
        p = buf + (size_t)((doc_cons_pos + i) % cap) * elem;
        for (uint32_t off = 0; off < elem; off += 32) {
            const uint32_t n = elem - off < 32 ? elem - off : 32;
            char hex[96];
            char *w = hex;
            for (uint32_t j = 0; j < n; j++) {
                *w++ = "0123456789abcdef"[p[off + j] >> 4];
                *w++ = "0123456789abcdef"[p[off + j] & 0xf];
            }
            *w = '\0';
            MC2VR_LOG("S1 elem @docConsPos +0x%02x: %s", i * elem + off, hex);
        }
    }
    p = buf + (size_t)(cons % cap) * elem;
    MC2VR_LOG("S1 elem @naiveCons +0x00: %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x "
              "%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
              p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
              p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15],
              p[16], p[17], p[18], p[19], p[20], p[21], p[22], p[23],
              p[24], p[25], p[26], p[27], p[28], p[29], p[30], p[31]);
    p = buf + (size_t)(prod % cap) * elem;
    MC2VR_LOG("S1 elem @naiveProd +0x00: %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x "
              "%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
              p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
              p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15],
              p[16], p[17], p[18], p[19], p[20], p[21], p[22], p[23],
              p[24], p[25], p[26], p[27], p[28], p[29], p[30], p[31]);
}

// ---- S1.4 per-frame view list (signature-deduped logging) --------------------

struct ViewRec {
    uint16_t idx;
    uint16_t type;
    uint32_t flags;
};
ViewRec g_list[FRAME_LIST_MAX];
uint32_t g_list_n = 0;
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

// ---- S1.3 residency patch ----------------------------------------------------

enum class PatchState { Idle, Active, Done };
PatchState g_patch_state = PatchState::Idle;
const uint8_t *g_patch_entry = nullptr;
float g_patch_saved[16];
uint32_t g_patch_left = 0;
uint32_t g_patch_tries = 0;
bool g_residency_seen = false;
bool g_residency_logged = false;

// ---- S1.2 xform/viewport aggregates -------------------------------------------

uint64_t g_x_calls = 0, g_x_view = 0, g_x_proj = 0, g_x_other = 0;
uint64_t g_match_entry = 0, g_match_entry_T = 0;
uint64_t g_match_ctx = 0, g_match_sub = 0, g_match_none = 0;
uint64_t g_x_detail = 0;
char g_last_view_tag[48] = "-";
char g_last_proj_tag[48] = "-";

uint64_t g_xf_frame = UINT64_MAX;
uint32_t g_xf_frame_n = 0;
uint32_t g_xf_min = 0xffffffff, g_xf_max = 0;

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

// ---- S1.2: SetTransform classification -----------------------------------------

void classify_matrix(const float *m, uint32_t state, uint64_t frame)
{
    char tagbuf[48];
    int32_t hit_idx = -1;
    uint32_t hit_k = 0;
    bool hit_t = false;

    if (find_in_entries(m, hit_idx, hit_k, hit_t)) {
        _snprintf(tagbuf, sizeof(tagbuf), "e%d.m%u%s", hit_idx, hit_k, hit_t ? "T" : "");
        if (hit_t) {
            g_match_entry_T++;
        } else {
            g_match_entry++;
        }

        // S1.3 residency proof: the patched live entry's m[1] showing up in a
        // SetTransform means the consumer derefs the live ViewEntry at
        // consume time (the whole clone-at-stage design rests on this).
        const uint8_t *hit_entry =
            (const uint8_t *)(VIEW_TABLE + (size_t)hit_idx * VIEW_STRIDE);
        if (!hit_t && hit_k == 1 && hit_entry == g_patch_entry &&
            g_patch_state == PatchState::Active) {
            g_residency_seen = true;
            if (!g_residency_logged) {
                g_residency_logged = true;
                MC2VR_LOG("S1 RESIDENCY PROVEN: SetTransform(state=%u) matches the live "
                          "PATCHED ViewEntry m[1] (delta %g) at frame=%llu — consumer "
                          "derefs the live entry at consume time",
                          state, (double)PATCH_DELTA, (unsigned long long)frame);
            }
        }
    } else {
        uint32_t off = 0;
        const uint8_t *block =
            g_ctx_valid ? (const uint8_t *)(g_frame_ctx + CTX_BLOCK_OFF) : nullptr;
        if (find_in_region(m, block, CTX_BLOCK_SIZE, off)) {
            _snprintf(tagbuf, sizeof(tagbuf), "ctx+0x%03x", off);
            g_match_ctx++;
        } else {
            bool sub_hit = false;
            for (uint32_t s = 0; block && s < 2 && !sub_hit; s++) {
                const uint8_t *sub = *(const uint8_t *const *)(block + CTX_SUBPTR_OFF + s * 4);
                if (find_in_region(m, sub, CTX_SUBOBJ_SCAN, off)) {
                    _snprintf(tagbuf, sizeof(tagbuf), "sub%u+0x%03x", s, off);
                    g_match_sub++;
                    sub_hit = true;
                }
            }
            if (!sub_hit) {
                strncpy(tagbuf, "none", sizeof(tagbuf) - 1);
                g_match_none++;
            }
        }
    }
    tagbuf[sizeof(tagbuf) - 1] = '\0';

    if (state == D3DTS_VIEW) {
        strncpy(g_last_view_tag, tagbuf, sizeof(g_last_view_tag) - 1);
        g_last_view_tag[sizeof(g_last_view_tag) - 1] = '\0';
    } else {
        strncpy(g_last_proj_tag, tagbuf, sizeof(g_last_proj_tag) - 1);
        g_last_proj_tag[sizeof(g_last_proj_tag) - 1] = '\0';
    }

    // One-shot detail lines: raw matrix bytes for offline diffing against
    // the M3 entry dumps (analyze_dumps.py decodes the hex to floats).
    if (g_x_detail < XFORM_DETAIL_BURST) {
        g_x_detail++;
        const uint8_t *raw = (const uint8_t *)m;
        char hex[132];
        char *w = hex;
        for (uint32_t i = 0; i < 64; i++) {
            *w++ = "0123456789abcdef"[raw[i] >> 4];
            *w++ = "0123456789abcdef"[raw[i] & 0xf];
        }
        *w = '\0';
        MC2VR_LOG("S1 xform: frame=%llu state=%s tag=%s m=%s",
                  (unsigned long long)frame, state == D3DTS_VIEW ? "view" : "proj",
                  tagbuf, hex);
    }
}

// ---- S1.4 helpers ---------------------------------------------------------------

void log_view_list(uint64_t frame)
{
    if (g_list_frame != frame || g_list_n == 0) {
        return; // no world views walked this frame (menu etc.)
    }
    g_list_frames++;

    // Signature over the records in walk order (order = activation recency,
    // head included — a change in either yields a new signature).
    uint32_t h = 0x811c9dc5;
    auto mix = [&h](uint32_t v) {
        h = (h ^ v) * 0x01000193;
    };
    mix((uint32_t)g_list_head);
    mix(g_list_n);
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

    const bool satellite = g_list_n > SATELLITE_MIN_VIEWS;
    if (satellite) {
        g_sat_frames++;
        if (g_list_n > g_sat_max) {
            g_sat_max = g_list_n;
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

    char list[900];
    int n = _snprintf(list, sizeof(list), "head=%d n=%u t2=%u sig=%08x ctx=%p:",
                      g_list_head, g_list_n, g_list_t2, h, (void *)g_frame_ctx);
    const uint32_t shown = g_list_n < LIST_LOG_ENTRIES ? g_list_n : LIST_LOG_ENTRIES;
    for (uint32_t i = 0; i < shown && n < (int)sizeof(list) - 24; i++) {
        n += _snprintf(list + n, sizeof(list) - n, " i%u/t%u/f%04x", g_list[i].idx,
                       g_list[i].type, g_list[i].flags >> 16);
    }
    if (g_list_n > shown && n < (int)sizeof(list) - 24) {
        _snprintf(list + n, sizeof(list) - n, " ...(%u more)", g_list_n - shown);
    }
    list[sizeof(list) - 1] = '\0';
    MC2VR_LOG("S1 vlist: frame=%llu %s", (unsigned long long)frame, list);
}

// ---- S1.3 residency patch driver -----------------------------------------------

void apply_patch(uint64_t frame)
{
    memcpy(g_patch_saved, g_patch_entry + VIEW_M1_OFF, sizeof(g_patch_saved));
    ((float *)(g_patch_entry + VIEW_M1_OFF))[12] += PATCH_DELTA;
    // g_patch_frame not needed: run_patch keys on frame != patch frame via
    // the call order (run_patch is called once per frame at the pre-VM site).
    (void)frame;
}

void run_patch(uint64_t frame)
{
    if (g_patch_state == PatchState::Done) {
        return;
    }
    if (g_patch_state == PatchState::Active) {
        // Restore the previous frame's m[1] first: the game may rewrite m[1]
        // at any point, so never keep a patch past its frame.
        memcpy((void *)(g_patch_entry + VIEW_M1_OFF), g_patch_saved, sizeof(g_patch_saved));
        if (--g_patch_left == 0) {
            g_patch_state = PatchState::Done;
            MC2VR_LOG("S1 patch: window ended (%u frames, delta %g) — residency_seen=%u",
                      (unsigned)PATCH_FRAMES, (double)PATCH_DELTA,
                      g_residency_seen ? 1u : 0u);
            return;
        }
        apply_patch(frame); // re-apply relative to the current values
        return;
    }

    // Idle: look for a type-2 head view to patch (head = the view the S3
    // duplication will care about first; type 2 = world view).
    if (++g_patch_tries > PATCH_TRY_MAX) {
        g_patch_state = PatchState::Done;
        MC2VR_LOG("S1 patch: no type-2 head view in %u frames — residency proof "
                  "skipped (menu-only run?)", (unsigned)PATCH_TRY_MAX);
        return;
    }
    const int32_t head = *(volatile int32_t *)MC2_VIEW_LIST_HEAD;
    if (head < 0 || (uint32_t)head >= VIEW_IDX_MAX) {
        return;
    }
    const uint8_t *entry = (const uint8_t *)(VIEW_TABLE + (size_t)head * VIEW_STRIDE);
    const uintptr_t ref = *(const uintptr_t *)(entry + MC2_VIEW_OBJ_PTR_OFF);
    if (!ref) {
        return;
    }
    if ((*(const uint32_t *)(ref + MC2_VIEW_REF_TYPEFLAGS_OFF) & 0xffff) != 2) {
        return; // head is not a world view this frame — try again next frame
    }
    g_patch_entry = entry;
    apply_patch(frame);
    g_patch_state = PatchState::Active;
    g_patch_left = PATCH_FRAMES; // frames left INCLUDING the current one: the
    // Active branch decrements at every later pre-hook, so the patch covers
    // exactly PATCH_FRAMES frames.
    MC2VR_LOG("S1 patch: head=%d entry=%p (ViewRef=%p) m1[3] += %g for %u frames — "
              "expected visual: brief world-camera nudge; proof: 'S1 RESIDENCY' line "
              "or e%d.m1 tag in S1 xform/window lines",
              head, (const void *)entry, (const void *)ref, (double)PATCH_DELTA,
              (unsigned)PATCH_FRAMES, head);
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
    s.cons_pre = ca & 0xffff;
    s.prod_pre = ca >> 16;
    s.seen |= 1;

    const uint32_t unconsumed = ((ca >> 16) - (ca & 0xffff)) & 0xffff;
    if (unconsumed < g_unc_min) {
        g_unc_min = unconsumed;
    }
    if (unconsumed > g_unc_max) {
        g_unc_max = unconsumed;
    }

    // One-shot element layout dump at the candidate consumer positions.
    if (g_elem_dumps < ELEM_DUMP_FRAMES && unconsumed > 0) {
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
    s.cons_rf = read_counters_a() & 0xffff;
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
        g_list_t2 = 0;
        g_list_head = *(volatile int32_t *)MC2_VIEW_LIST_HEAD;
    }
    if (g_list_n < FRAME_LIST_MAX) {
        g_list[g_list_n++] = {(uint16_t)idx, (uint16_t)type, flags};
    }
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
    s.cons_cmd = read_counters_a() & 0xffff;
}

void note_end_of_frame()
{
    FrameSample &s = sample_for(hooks::frame_count());
    s.seen |= 8;
    s.cons_eof = read_counters_a() & 0xffff;
}

void on_set_transform(uint32_t state, const float *m)
{
    g_x_calls++;
    if (!m) {
        g_x_other++;
        return;
    }
    if (state != D3DTS_VIEW && state != D3DTS_PROJECTION) {
        g_x_other++; // world/texture matrices: counted only (hot path)
        return;
    }

    const uint64_t frame = hooks::frame_count();
    if (frame != g_xf_frame) {
        if (g_xf_frame != UINT64_MAX) {
            if (g_xf_frame_n < g_xf_min) {
                g_xf_min = g_xf_frame_n;
            }
            if (g_xf_frame_n > g_xf_max) {
                g_xf_max = g_xf_frame_n;
            }
        }
        g_xf_frame = frame;
        g_xf_frame_n = 0;
    }
    g_xf_frame_n++;

    if (state == D3DTS_VIEW) {
        g_x_view++;
    } else {
        g_x_proj++;
    }
    classify_matrix(m, state, frame);
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
              "none=%llu | unconsumedAtPre min=%u max=%u",
              (unsigned long long)g_full_brackets, (unsigned long long)g_adv_pipeline,
              (unsigned long long)g_adv_rf_to_cmd, (unsigned long long)g_adv_cmd_to_eof,
              (unsigned long long)g_adv_none, g_unc_min == 0xffffffff ? 0 : g_unc_min,
              g_unc_max);

    MC2VR_LOG("S1 xform: calls=%llu view=%llu proj=%llu other=%llu | matches "
              "entry=%llu entryT=%llu ctx=%llu sub=%llu none=%llu | viewTag=%s "
              "projTag=%s | view+proj sets/frame min=%u max=%u | residency=%s",
              (unsigned long long)g_x_calls, (unsigned long long)g_x_view,
              (unsigned long long)g_x_proj, (unsigned long long)g_x_other,
              (unsigned long long)g_match_entry, (unsigned long long)g_match_entry_T,
              (unsigned long long)g_match_ctx, (unsigned long long)g_match_sub,
              (unsigned long long)g_match_none, g_last_view_tag, g_last_proj_tag,
              g_xf_min == 0xffffffff ? 0 : g_xf_min, g_xf_max,
              g_residency_seen ? "PROVEN" : "not-yet");

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
    // g_elem_dumps, g_x_detail, g_vp_burst, g_list_first_logged,
    // g_residency_seen/logged, g_patch_*) are NOT reset.
    g_full_brackets = 0;
    g_adv_pipeline = 0;
    g_adv_rf_to_cmd = 0;
    g_adv_cmd_to_eof = 0;
    g_adv_none = 0;
    g_unc_min = 0xffffffff;
    g_unc_max = 0;
    g_x_calls = 0;
    g_x_view = 0;
    g_x_proj = 0;
    g_x_other = 0;
    g_match_entry = 0;
    g_match_entry_T = 0;
    g_match_ctx = 0;
    g_match_sub = 0;
    g_match_none = 0;
    strncpy(g_last_view_tag, "-", sizeof(g_last_view_tag) - 1);
    strncpy(g_last_proj_tag, "-", sizeof(g_last_proj_tag) - 1);
    g_xf_min = 0xffffffff;
    g_xf_max = 0;
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
        MC2VR_LOG("S1: installed RenderFrame-entry MidHook @ %p", (void *)MC2_RENDERSHELL_RENDERFRAME);
    }
}

} // namespace mc2vr::s1
