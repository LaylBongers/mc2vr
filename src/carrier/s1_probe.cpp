// S1c — third revision of the S1 instrumentation. Run history:
//   S1  (run 1): consumer bracket + SetTransform/SetViewport logging + m[1]
//        residency patch + satellite classification. ANSWERS: consumer runs
//        at pipeline time; elements are {size,ptr} descriptors; SetTransform
//        NEVER called (shader-driven); satellite frames confirmed; the S0
//        counter decode was wrong.
//   S1b (run 2): SetVertexShaderConstantF (slot 94) classification + live-head
//        patch windows. ANSWERS: world-view element {0x30,0x810,0x680} VERIFIED
//        in the ring (staging/entry/ctx pointers all correct); ring position =
//        A.low16 % cap (advancing 624/frame in the boat scene; mid-frame reads
//        are VM scratch — high16 mutates transiently); GPU-bound matrices do
//        NOT exactly match any ViewEntry matrix (derived, not copied); patch
//        windows A(m[1])/B(staging slot) on the only rendered view produced
//        NO nudge -> neither channel feeds the draw camera. REGRESSION: the
//        per-group classification (up to 3.6M/10s x ~800 memcmps) halved the
//        frame rate -> S1c memoizes and budgets it.
//   S1c (run 3, this): performance fix (content-hash memo + per-frame
//        classification/region budgets); matched-tag logging (run 2's single
//        real match — 4x transposed on c12 — had no tag logged); patch
//        windows A-E across ALL walked views and ALL candidate camera
//        channels: A entry m[1][3], B camera staging slot pos (ctx+0xc2110),
//        C camera-record ring records (ctx+0xcb110, {pos,serial,rot,entry*,lod}
//        0x28-stride — the walk-time snapshot the VM consumer most plausibly
//        reads), D entry pos7c4[0], E entry m[0][12]; a full-ring SCAN for
//        {0x30,0x810,0x680} world elements (no more position guessing); and a
//        bounded one-shot matrix EXFIL (unique GPU matrices + walked-view
//        m[0]/m[1] hex + FOV dwords) for offline derivation analysis — the
//        GPU matrices are derived, so exact-match classification can only ever
//        attribute a subset; the exfil lets the analyzer search the
//        relationships offline.
// New MidHooks live here; the M3 handlers (render_dump.cpp) and the device
// VmtHook (device.cpp) feed this module via the note_*/on_* taps. Handlers
// run on the main thread; report_window() runs on the queue poller thread.

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
constexpr uintptr_t VIEW_TABLE_END = VIEW_TABLE + (size_t)VIEW_IDX_MAX * VIEW_STRIDE;

// Entry camera-position copies: +0x7c4 is read by the walk's staging copy
// (0x0048ec3e); +0x7ac is the OTHER copy (M3 field map) — never patched
// before S1f (window H).
constexpr uint32_t VIEW_POS_OFF = 0x7c4;
constexpr uint32_t VIEW_POS2_OFF = 0x7ac;

// Frame-ctx layout (SubmitWorldPackets this+...): 0x680 block at +0xd2950
// (S0), per-view camera staging slots at +0xc2110 (stride 0x30; content per
// walk: pos3 at +0x00, serial +0x0c, rot16 +0x10, lodByte +0x20), and the
// walk-time camera-record ring at +0xcb110 (0x28-stride records
// {pos3, serial, rot16, ViewEntry* at +0x20, lodByte}; published per walk by
// FUN_004906b0, reader VM-hidden). The ring scan is bounded to +0xd2950 so
// it never leaves the known object extent.
constexpr uint32_t CTX_BLOCK_OFF = 0xd2950;
constexpr uint32_t CTX_BLOCK_SIZE = 0x680;
constexpr uint32_t CTX_QUEUEPTR_OFF = 0x60;  // &g_RenderQueue (also +0x6c/+0x9c/+0xa8)
constexpr uint32_t CTX_TABLEPTR_OFF = 0xC4; // g_ViewTable
constexpr uint32_t CTX_SUBPTR_OFF = 0x74;    // resolved primary-subobject ptrs[2]
constexpr uint32_t CTX_SUBOBJ_SCAN = 0x3a0; // live primary-subobject size
constexpr uint32_t CTX_STAGING_OFF = 0xc2110;
constexpr uint32_t STAGING_STRIDE = 0x30;
constexpr uint32_t CTX_RING_OFF = 0xcb110;
constexpr uint32_t RING_REC_STRIDE = 0x28;
// Run-4 "S1 crec:" evidence: record = {pos-ish 2 floats +0x00/+0x04,
// quaternion +0x08..+0x14, ViewEntry* at +0x18, flags +0x1c, 2 floats
// +0x20/+0x24}; records for consecutive walked views sit consecutively.
// (The S0 plate comment's {pos3,serial,rot16,entry*} layout put the ptr at
// +0x20 — wrong; window C starved on it.)
constexpr uint32_t RING_REC_ENTRYPTR_OFF = 0x18;

constexpr uint32_t D3DTS_VIEW = 2;
constexpr uint32_t D3DTS_PROJECTION = 3;

// ---- logging budget knobs ---------------------------------------------------

constexpr uint32_t FRAME_SAMPLES = 8;      // per-frame bracket ring (frame % 8)
constexpr uint32_t BRACKET_BURST = 3;      // one-shot full bracket lines
constexpr uint32_t RAW_SERIES_MAX = 60;     // one-shot raw-counter lines (world-view frames)
constexpr uint32_t ELEM_DUMP_FRAMES = 2;    // full-ring world-element scans
constexpr uint32_t ELEM_DUMP_FOUND_MAX = 4; // world elements dumped per scan
constexpr uint32_t FRAME_LIST_MAX = 64;     // per-frame (idx,type,flags) recs
constexpr uint32_t LIST_LOG_ENTRIES = 40;   // entries in a full-list log line
constexpr uint32_t SIG_SLOTS = 6;          // distinct view-list signatures / window
constexpr uint32_t RECT_SLOTS = 16;        // distinct viewport rects / window
constexpr uint32_t VP_BURST = 3;           // one-shot full viewport lines
constexpr uint32_t SATELLITE_MIN_VIEWS = 100;
constexpr uint32_t SATELLITE_LOG_MAX = 2;   // full satellite lists / window
constexpr uint32_t VS_ROWS = 256;           // VS constant float4 cache (vs_2_0 max)

// S1c performance knobs (run-2 regression: ~13k classifications/frame x
// ~800 memcmps halved the frame rate).
constexpr uint32_t VS_BULK_MAX_GROUPS = 64;  // per-call cap on bulk-aligned groups
constexpr uint32_t VS_CLS_BUDGET = 256;      // NEW matrix classifications / frame
constexpr uint32_t VS_REGION_BUDGET = 64;   // of those, how many may do the
                                            // expensive ctx/subobject region scans
constexpr uint32_t VS_MEMO_SLOTS = 512;      // content-hash memo (direct-mapped)
constexpr uint32_t VS_DETAIL_BURST = 8;     // one-shot vsmat detail lines
constexpr uint32_t VS_MATCH_LOG_MAX = 32;    // one-shot vsmatch (reg,tag) lines

// S1c/S1e evidence exfil (offline derivation analysis of the GPU matrices).
constexpr uint32_t EXFIL_FRAMES = 12;        // total exfil frames
constexpr uint32_t EXFIL_EARLY = 4;         // first N world-view frames
constexpr uint32_t EXFIL_STRIDE = 200;       // then every Nth world-view frame
constexpr uint32_t EXFIL_MATS = 48;          // unique GPU matrices per frame
constexpr uint32_t EXFIL_VIEWS = 6;         // walked views with m0/m1 hex
constexpr uint32_t EXFIL_FULL_VIEWS = 3;     // first walked views with ALL 9
constexpr uint32_t EXFIL_SUB_SIZE = 0x164;  // ctx-block subobject copies
constexpr uint32_t EXFIL_SUB_COUNT = 2;     // at block +0xEC

// S1c residency patch windows: five candidate camera channels, patched one
// window at a time on ALL walked views, 5 frames each, ~1s apart so the
// nudges are separately visible. Run-2 result: windows A/B (head view) had
// no nudge; S1c re-runs them on all views plus the three remaining channels.
constexpr float PATCH_DELTA = 4.0f;
constexpr uint32_t PATCH_WINDOW = 5;
constexpr uint32_t PATCH_GAP = 60;
constexpr uint32_t PATCH_MAX_TARGETS = 160; // stage F needs 7/view x ~20 views
constexpr uint32_t PATCH_MAX_RECORDS = 128;
// Run-3 lessons: gameplay submits 15-85 world views, menu/cutscene
// backgrounds 1-2 — gate window starts on gameplay-like frames so the
// sequence does not fire on a menu background and get its budget burned by
// the level-load screen (which submits no views for ~35s).
constexpr uint32_t PATCH_GAMEPLAY_MIN_VIEWS = 4;
constexpr uint32_t PATCH_STAGE_TRIES = 600; // per-stage retries before advancing
constexpr float PATCH_POS_EPS = 1e-3f;       // window G pos-match epsilon
constexpr uint32_t PATCH_POS_TARGETS = 96;   // live-view pos components to scan for
constexpr uint32_t PATCH_EXP_MAX = 160;     // patched-matrix expected set (GPU proof)
// Camera/view constants live in LOW registers; classify those ahead of the
// budget (run 3: ~24k float4s/frame starved a flat 256/frame budget on
// per-object matrices before the camera registers got a look).
constexpr uint32_t VS_PRIOR_REGS = 32;

// ---- hook storage (leaked by design, same teardown reasoning as M2/M3) ------

SafetyHookMid g_pre_vm_mid;
SafetyHookMid g_renderframe_mid;

// ---- S1.1 consumer bracket ---------------------------------------------------
// queue+0x10 runtime decode (run 2): low16 = ring POSITION (mod cap,
// +624/frame boat scene); high16 = VM scratch (mutated transiently mid-frame,
// spikes to hundreds at RenderFrame entry); +0x14 stays ~0-2. The bracket's
// value is "the header dword changed between pre-VM and RenderFrame entry" =
// the VM'd call does queue work at pipeline time; per-point deltas are noise.

struct FrameSample {
    uint64_t frame = UINT64_MAX;
    uint32_t a_pre = 0, a_rf = 0, a_cmd = 0, a_eof = 0; // queue+0x10 raw dwords
    uint32_t b_pre = 0;                                 // queue+0x14
    uint32_t full_delta = 0;   // pre(N) - pre(N-1): the meaningful one
    uint32_t views = 0;         // world views walked this frame (total)
    uint8_t seen = 0;           // bit0 pre, 1 rf, 2 cmd, 3 eof
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

void classify_sample(FrameSample &s)
{
    if (s.classified || (s.seen & 0x3) != 0x3) {
        return; // needs pre + rf at minimum
    }
    s.classified = true;
    g_full_brackets++;

    // "Changed" counts (the VM mutates the header at these points; nonzero
    // delta = the VM'd call/RenderFrame did queue work there).
    const uint32_t d_pipe = s.a_rf - s.a_pre;
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
                  "eof=%08x B=%08x | changed pre->rf=%u rf->cmd=%u cmd->eof=%u "
                  "fullFrame=%u",
                  (unsigned long long)s.frame, s.views, s.a_pre, s.a_rf, s.a_cmd,
                  s.a_eof, s.b_pre, d_pipe, d_cmd, d_eof, s.full_delta);
    }

    // One-shot raw-counter series over world-view frames (offline decode basis).
    if (s.views > 0 && g_raw_series < RAW_SERIES_MAX) {
        g_raw_series++;
        MC2VR_LOG("S1 raw: frame=%llu views=%u A pre=%08x rf=%08x cmd=%08x "
                  "eof=%08x B=%08x | d(pre->rf)=%u d(rf->cmd)=%u d(cmd->eof)=%u "
                  "fullFrame=%u",
                  (unsigned long long)s.frame, s.views, s.a_pre, s.a_rf, s.a_cmd,
                  s.a_eof, s.b_pre, d_pipe, d_cmd, d_eof, s.full_delta);
    }
}

// ---- S1.1 ring scan for world-view elements (no position guessing) ------------

bool queue_fields_sane(uint32_t &elem, uint32_t &cap, const uint8_t *&buf)
{
    elem = *(const uint32_t *)MC2_QUEUE_ELEM_SIZE;
    cap = *(const uint32_t *)MC2_QUEUE_CAPACITY;
    buf = *(const uint8_t *const *)MC2_QUEUE_BUFFER;
    // +0xc must be a POINTER (the counters sit at +0x10, so the ring cannot
    // be inline); still guard the range before dereferencing it.
    return buf && (uintptr_t)buf >= 0x00100000 && (uintptr_t)buf < 0x80000000 &&
           elem > 0 && elem <= 256 && cap > 0 && cap <= 65536 &&
           (uint64_t)elem * cap <= 64ull * 1024 * 1024;
}

void dump_element_lines(const char *tag, const uint8_t *p, uint32_t elem)
{
    for (uint32_t off = 0; off < elem; off += 32) {
        const uint32_t n = elem - off < 32 ? elem - off : 32;
        char hex[96];
        char *w = hex;
        for (uint32_t j = 0; j < n; j++) {
            *w++ = "0123456789abcdef"[p[off + j] >> 4];
            *w++ = "0123456789abcdef"[p[off + j] & 0xf];
        }
        *w = '\0';
        MC2VR_LOG("S1 elem @%s +0x%02x: %s", tag, off, hex);
    }
}

void scan_ring_world_elements(uint32_t a, uint32_t b)
{
    uint32_t elem, cap;
    const uint8_t *buf;
    if (!queue_fields_sane(elem, cap, buf)) {
        MC2VR_LOG("S1 elemscan: queue fields not sane (elem=%u cap=%u) — scan skipped",
                  elem, cap);
        return;
    }
    MC2VR_LOG("S1 elemscan: A=%08x B=%08x buf=%p pos=%u — scanning full ring for "
              "{0x30,0x810,0x680} world elements",
              a, b, (const void *)buf, (a & 0xffff) % cap);

    // The world element's pair1 sits at element+0x24; scan every aligned
    // dword for size 0x810 + a pointer into the view table, with 0x30 at
    // -8 and 0x680 at +8 as confirmation.
    uint32_t found = 0;
    const uint8_t *scan_end = buf + (size_t)cap * elem - 0x28;
    for (const uint8_t *p = buf + 0x24; p < scan_end && found < ELEM_DUMP_FOUND_MAX;
         p += 4) {
        if (*(const uint32_t *)p != 0x810) {
            continue;
        }
        const uintptr_t ep = *(const uintptr_t *)(p + 4);
        if (ep < VIEW_TABLE || ep >= VIEW_TABLE_END) {
            continue;
        }
        if (*(const uint32_t *)(p - 8) != 0x30 || *(const uint32_t *)(p + 8) != 0x680) {
            continue;
        }
        const uint8_t *base = p - 0x24;
        MC2VR_LOG("S1 elem @scan%u: world element (ViewEntry=%p)", found,
                  (const void *)ep);
        dump_element_lines("scan", base, elem);
        found++;
    }
    if (found == 0) {
        MC2VR_LOG("S1 elemscan: no world element in the ring this frame");
    }
}

// ---- S1.4 per-frame view list ------------------------------------------------

struct ViewRec {
    uint16_t idx;
    uint16_t type;
    uint32_t flags;
};
ViewRec g_list[FRAME_LIST_MAX];
uint32_t g_list_n = 0;        // capped record count (log line + target build)
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
uint64_t g_list_first_logged = 0;
uint64_t g_sat_logged_window = 0;

// Window aggregates.
uint64_t g_list_frames = 0;
uint64_t g_sat_frames = 0;
uint32_t g_sat_max = 0;
uint32_t g_t2_min = 0xffffffff, g_t2_max = 0;

bool type_flags_mismatch_logged = false;

// ---- S1.3/S1c residency patch windows A-E ---------------------------------------

enum class PatchState { Idle, Window, Gap, Done };
PatchState g_pstate = PatchState::Idle;
int g_pstage = 0; // 0=A m[1][3], 1=B staging pos, 2=C ring record pos,
                  // 3=D entry pos7c4[0], 4=E m[0][12], 5=F m[2..m[8][12],
                  // 6=G ctx-block/live-subobject pos matches, 7=H pos7ac[0]
const char *const PATCH_NAMES[8] = {
    "A entry m[1][3]", "B staging-slot pos[0]", "C camera-ring record pos[0]",
    "D entry pos7c4[0]", "E entry m[0][12]", "F entry m[2..m[8][12]",
    "G ctx/live-sub pos matches", "H entry pos7ac[0]",
};

uint8_t *g_pt_addr[PATCH_MAX_TARGETS];
float g_pt_saved[PATCH_MAX_TARGETS];
uint32_t g_pt_idx[PATCH_MAX_TARGETS];
uint32_t g_pt_k[PATCH_MAX_TARGETS]; // matrix index for stages 0/4/5 (GPU proof)
uint32_t g_pt_n = 0;

// GPU proof (S1f — the "is the experiment wired" check): during matrix patch
// windows (A/E/F) every GPU-bound group is compared against the exact
// PATCHED matrices; a hit proves the field reaches the GPU even when the
// visible result is nil (offscreen view). Silence proves the consumer does
// not read the field at all.
float g_exp_mat[PATCH_EXP_MAX][16];
uint8_t g_exp_hit_logged[PATCH_EXP_MAX];
uint32_t g_exp_n = 0;

// Window G: live-view camera position components to scan for in the ctx
// block and the live primary subobjects.
float g_pos_tgts[PATCH_POS_TARGETS];
uint32_t g_pos_tgt_n = 0;
uint32_t g_pleft = 0;
uint32_t g_pgap = 0;
uint32_t g_pstage_tries = 0;
bool g_patch_any_live = false;
bool g_crec_dumped = false;

bool g_residency_seen = false;
bool g_residency_logged = false;

// ---- S1.2/S1c VS-constant channel ----------------------------------------------

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

uint64_t g_vs_calls = 0, g_vs_vec4s = 0;
uint64_t g_vs_classified = 0;   // total classification attempts (incl. memo hits)
uint64_t g_vs_cls_new = 0;       // actual find_matrix runs
uint64_t g_vs_cls_skipped = 0;   // budget-exceeded skips
uint64_t g_vs_memo_hits = 0;
uint64_t g_vs_detail = 0;
uint64_t g_vs_reg_hits[VS_ROWS] = {};
uint64_t g_vs_frame_key = UINT64_MAX;
uint32_t g_vs_cls_budget_left = VS_CLS_BUDGET;
uint32_t g_vs_region_budget_left = VS_REGION_BUDGET;
float g_vs_rows[VS_ROWS][4] = {};
uint8_t g_vs_valid[VS_ROWS] = {};

struct VsMatchAgg {
    uint64_t entry, entryT, ctx, sub, none;
};
VsMatchAgg g_vs{};

// Match-tag one-shot logging (run-2 lesson: the single real match — 4x
// transposed on c12 — never got a tag logged).
struct MatchLog {
    uint32_t reg;
    char tag[40];
};
MatchLog g_match_log[VS_MATCH_LOG_MAX];
uint32_t g_match_log_n = 0;

// ---- S1c evidence exfil ---------------------------------------------------------

uint32_t g_exfil_done = 0;
uint32_t g_exfil_view_counter = 0;
struct ExMat {
    uint32_t hash;
    uint32_t reg;
    uint8_t raw[64];
};
ExMat g_exmats[EXFIL_MATS];
uint32_t g_exmat_n = 0;
// Entry snapshot taken at SELECTION time (the walk of that frame just
// finished, so entry values are final); the GPU matrices arrive during the
// same frame's render. Run-3 bug: the emit keyed off the LIVE view list,
// which has already moved on by emit time — nothing was ever logged.
struct ExEntrySnap {
    uint32_t idx;
    uint8_t m0[64];
    uint8_t m1[64];
    uint8_t has_all;
    uint8_t mall[VIEW_MATRIX_COUNT * VIEW_MATRIX_STRIDE];
};
ExEntrySnap g_ex_entries[EXFIL_VIEWS];
uint32_t g_ex_entry_n = 0;
uint32_t g_ex_fov[6] = {};
uint32_t g_ex_pending = 0;
uint64_t g_ex_frame = 0;
uint32_t g_ex_views_total = 0;

// ---- helpers ---------------------------------------------------------------------

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

uint32_t hash64(const uint8_t *p)
{
    uint32_t h = 0x811c9dc5;
    for (uint32_t i = 0; i < 16; i++) {
        h = (h ^ *(const uint32_t *)(p + i * 4)) * 0x01000193;
    }
    return h;
}

void hex64(const void *m, char *out, size_t out_size)
{
    const uint8_t *raw = (const uint8_t *)m;
    char *w = out;
    for (uint32_t i = 0; i < 64 && (size_t)(w - out) + 2 < out_size; i++) {
        *w++ = "0123456789abcdef"[raw[i] >> 4];
        *w++ = "0123456789abcdef"[raw[i] & 0xf];
    }
    *w = '\0';
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

// Shared classification core. `region_scan` gates the expensive ctx/subobject
// scans (S1c performance fix: entries are cheap to scan every time, the
// region scans are budgeted).
struct MatrixMatch {
    enum class Kind { None, Entry, Ctx, Sub } kind = Kind::None;
    const uint8_t *entry = nullptr; // Kind::Entry
    int32_t idx = -1;
    uint32_t k = 0;
    bool transposed = false;
    uint32_t region_off = 0; // Kind::Ctx / Kind::Sub
    uint32_t sub_idx = 0;     // Kind::Sub
};

MatrixMatch find_matrix(const float *m, bool region_scan)
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
    if (!region_scan) {
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
        if (out_size) {
            strncpy(out, "none", out_size - 1);
            out[out_size - 1] = '\0';
        }
        break;
    }
    if (out_size) {
        out[out_size - 1] = '\0';
    }
}

// Residency proof: a matrix equal to the live PATCHED entry m[1] reaching the
// GPU means the consumer derefs the live ViewEntry at consume time.
void check_residency(const MatrixMatch &mm)
{
    if (mm.kind != MatrixMatch::Kind::Entry || mm.transposed || mm.k != 1 ||
        g_pstate != PatchState::Window || g_pstage != 0) {
        return;
    }
    // Window A targets are entry+M1_OFF+48; verify the match is one of them.
    for (uint32_t i = 0; i < g_pt_n; i++) {
        const uint8_t *hit_entry = (const uint8_t *)(VIEW_TABLE + (size_t)mm.idx * VIEW_STRIDE);
        if (g_pt_addr[i] - VIEW_M1_OFF == hit_entry) {
            g_residency_seen = true;
            if (!g_residency_logged) {
                g_residency_logged = true;
                MC2VR_LOG("S1 RESIDENCY PROVEN: GPU-bound matrix matches the live "
                          "PATCHED ViewEntry m[1] of idx=%d (delta %g) — the "
                          "consumer derefs the live entry at consume time",
                          mm.idx, (double)PATCH_DELTA);
            }
            return;
        }
    }
}

// ---- S1.4 helpers ------------------------------------------------------------

void log_view_list(uint64_t frame)
{
    if (g_list_frame != frame || g_list_total == 0) {
        return;
    }
    g_list_frames++;

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

// ---- S1c patch windows A-E -------------------------------------------------------

bool view_is_live(const uint8_t *entry)
{
    const float *m0 = (const float *)(entry + VIEW_M0_OFF);
    return m0[12] != 0.0f || m0[13] != 0.0f || m0[14] != 0.0f;
}

// Window C: scan the walk-time camera-record ring for records whose
// ViewEntry* matches a walked view; collect them as patch targets.
bool window_c_scan()
{
    if (!g_ctx_valid) {
        return false;
    }
    g_pt_n = 0;
    const uint8_t *base = (const uint8_t *)(g_frame_ctx + CTX_RING_OFF);
    const uint8_t *end = (const uint8_t *)(g_frame_ctx + CTX_BLOCK_OFF);
    for (const uint8_t *r = base;
         r + RING_REC_STRIDE <= end && g_pt_n < PATCH_MAX_RECORDS;
         r += RING_REC_STRIDE) {
        const uintptr_t ep = *(const uintptr_t *)(r + RING_REC_ENTRYPTR_OFF);
        if (ep < VIEW_TABLE || ep >= VIEW_TABLE_END) {
            continue;
        }
        uint32_t matched_idx = 0xffffffffu;
        for (uint32_t i = 0; i < g_list_n; i++) {
            const uint32_t idx = g_list[i].idx;
            if (idx < VIEW_IDX_MAX &&
                (uintptr_t)(VIEW_TABLE + (size_t)idx * VIEW_STRIDE) == ep) {
                matched_idx = idx;
                break;
            }
        }
        if (matched_idx == 0xffffffffu) {
            continue;
        }
        // Stale matches (already-consumed records of prior frames) are
        // harmless: the fresh record for this frame is what the consumer reads.
        g_pt_addr[g_pt_n] = (uint8_t *)r;
        g_pt_saved[g_pt_n] = *(const float *)r;
        g_pt_idx[g_pt_n] = matched_idx;
        g_pt_k[g_pt_n] = 0;
        g_pt_n++;
    }
    if (g_pt_n == 0) {
        return false;
    }
    g_patch_any_live = true;
    return true;
}

// Window G: scan a byte range for floats equal (within PATCH_POS_EPS) to any
// live walked view's camera position component — the surgical way to find
// wherever the current camera coordinates live inside the ctx block and the
// live primary subobjects (the last VM-visible camera homes, untouched by
// windows A-F).
void g_scan_range(const uint8_t *base, uint32_t size)
{
    if (!base || (uintptr_t)base < 0x00100000 || (uintptr_t)base >= 0x80000000) {
        return;
    }
    for (uint32_t off = 0; off + 4 <= size && g_pt_n < PATCH_MAX_TARGETS; off += 4) {
        const float v = *(const float *)(base + off);
        if (v == 0.0f) {
            continue;
        }
        for (uint32_t i = 0; i < g_pos_tgt_n; i++) {
            const float d = v - g_pos_tgts[i];
            if (d < PATCH_POS_EPS && d > -PATCH_POS_EPS) {
                g_pt_addr[g_pt_n] = (uint8_t *)(base + off);
                g_pt_saved[g_pt_n] = v;
                g_pt_idx[g_pt_n] = 0xffffffffu; // range target, not a view
                g_pt_k[g_pt_n] = 0;
                g_pt_n++;
                break;
            }
        }
    }
}

bool window_g_scan()
{
    if (!g_ctx_valid) {
        return false;
    }
    g_pt_n = 0;
    g_pos_tgt_n = 0;
    // Collect live views' pos7c4 components.
    for (uint32_t i = 0; i < g_list_n && g_pos_tgt_n + 3 <= PATCH_POS_TARGETS; i++) {
        const uint32_t idx = g_list[i].idx;
        if (idx >= VIEW_IDX_MAX) {
            continue;
        }
        const uint8_t *entry = (const uint8_t *)(VIEW_TABLE + (size_t)idx * VIEW_STRIDE);
        if (!view_is_live(entry)) {
            continue;
        }
        const float *pos = (const float *)(entry + VIEW_POS_OFF);
        if (pos[0] == 0.0f && pos[1] == 0.0f && pos[2] == 0.0f) {
            continue;
        }
        g_pos_tgts[g_pos_tgt_n++] = pos[0];
        g_pos_tgts[g_pos_tgt_n++] = pos[1];
        g_pos_tgts[g_pos_tgt_n++] = pos[2];
    }
    if (g_pos_tgt_n == 0) {
        return false;
    }
    const uint8_t *block = (const uint8_t *)(g_frame_ctx + CTX_BLOCK_OFF);
    g_scan_range(block, CTX_BLOCK_SIZE); // incl. the +0xEC subobject copies
    for (uint32_t s = 0; s < 2; s++) {
        g_scan_range(*(const uint8_t *const *)(block + CTX_SUBPTR_OFF + s * 4),
                     CTX_SUBOBJ_SCAN); // the LIVE primary subobjects
    }
    if (g_pt_n == 0) {
        return false;
    }
    g_patch_any_live = true;
    return true;
}

// Build the target list for the current window stage from THIS frame's walked
// views. Returns false if no targets could be built (caller retries later).
bool build_patch_targets(uint64_t frame)
{
    g_pt_n = 0;
    g_patch_any_live = false;
    if (g_list_frame != frame || g_list_n == 0) {
        return false;
    }
    if (g_pstage == 2) {
        return window_c_scan(); // C targets the ring, not per-view fields
    }
    if (g_pstage == 6) {
        return window_g_scan(); // G targets ctx-block/live-subobject pos floats
    }
    for (uint32_t i = 0; i < g_list_n && g_pt_n < PATCH_MAX_TARGETS; i++) {
        const uint32_t idx = g_list[i].idx;
        if (idx >= VIEW_IDX_MAX) {
            continue;
        }
        const uint8_t *entry = (const uint8_t *)(VIEW_TABLE + (size_t)idx * VIEW_STRIDE);
        const bool live = view_is_live(entry);
        uint8_t *addr = nullptr;
        switch (g_pstage) {
        case 0: // A: entry m[1][3]
            if (!live) {
                continue;
            }
            addr = (uint8_t *)(entry + VIEW_M1_OFF + 12 * 4);
            break;
        case 1: // B: camera staging slot pos[0]
            if (!g_ctx_valid) {
                return false;
            }
            addr = (uint8_t *)(g_frame_ctx + CTX_STAGING_OFF + (size_t)idx * STAGING_STRIDE);
            break;
        case 3: // D: entry pos7c4[0]
            if (!live) {
                continue;
            }
            addr = (uint8_t *)(entry + VIEW_POS_OFF);
            break;
        case 7: // H: entry pos7ac[0] — the OTHER M3 camera position copy
            if (!live) {
                continue;
            }
            addr = (uint8_t *)(entry + VIEW_POS2_OFF);
            break;
        case 4: // E: entry m[0][12]
            if (!live) {
                continue;
            }
            addr = (uint8_t *)(entry + VIEW_M0_OFF + 12 * 4);
            break;
        case 5: { // F: entry m[2..m[8][12] — the never-patched matrices
            // (run-4 evidence: satellite views' m[0]/m[6] reach the GPU
            // exact; the main camera uses derived data — maybe from one of
            // the other seven). One target per matrix; the loop below takes
            // ONE addr, so stage F pushes its 7 targets directly.
            if (!live) {
                continue;
            }
            for (uint32_t k = 2; k < VIEW_MATRIX_COUNT && g_pt_n < PATCH_MAX_TARGETS;
                 k++) {
                uint8_t *a = (uint8_t *)(entry + VIEW_MATRIX_OFF +
                                         k * VIEW_MATRIX_STRIDE + 12 * 4);
                g_pt_addr[g_pt_n] = a;
                g_pt_saved[g_pt_n] = *(const float *)a;
                g_pt_idx[g_pt_n] = idx;
                g_pt_k[g_pt_n] = k;
                if (live) {
                    g_patch_any_live = true;
                }
                g_pt_n++;
            }
            continue;
        }
        default:
            return false;
        }
        g_pt_addr[g_pt_n] = addr;
        g_pt_saved[g_pt_n] = *(const float *)addr;
        g_pt_idx[g_pt_n] = idx;
        g_pt_k[g_pt_n] = (g_pstage == 0) ? 1 : (g_pstage == 4 ? 0 : 0);
        if (live) {
            g_patch_any_live = true;
        }
        g_pt_n++;
    }
    return g_pt_n > 0;
}

void patch_restore_targets()
{
    for (uint32_t i = 0; i < g_pt_n; i++) {
        if (g_pstage == 2 || g_pstage == 6) {
            // Ring records / ctx-subobject floats keep getting rewritten:
            // only restore a slot that still holds OUR patch (else it was
            // reused for fresh data).
            if (*(const float *)g_pt_addr[i] == g_pt_saved[i] + PATCH_DELTA) {
                *(float *)g_pt_addr[i] = g_pt_saved[i];
            }
        } else {
            *(float *)g_pt_addr[i] = g_pt_saved[i];
        }
    }
}

void patch_apply_targets()
{
    for (uint32_t i = 0; i < g_pt_n; i++) {
        // Re-read the current value each frame (the game rewrites these
        // fields between frames) and patch relative to it.
        g_pt_saved[i] = *(const float *)g_pt_addr[i];
        *(float *)g_pt_addr[i] = g_pt_saved[i] + PATCH_DELTA;
    }
    // GPU proof set (stages A/E/F): the exact patched matrices, read back
    // after patching. Any GPU upload equal to one of these proves the
    // channel reaches the GPU (S1f "is the experiment wired" check).
    g_exp_n = 0;
    if (g_pstage == 0 || g_pstage == 4 || g_pstage == 5) {
        for (uint32_t i = 0; i < g_pt_n && g_exp_n < PATCH_EXP_MAX; i++) {
            if (g_pt_idx[i] == 0xffffffffu) {
                continue; // range targets (window G) have no matrix
            }
            const uint8_t *mat = g_pt_addr[i] - 12 * 4; // matrix base (target is [12])
            memcpy(g_exp_mat[g_exp_n], mat, 64);
            g_exp_mat[g_exp_n][12] += PATCH_DELTA;
            g_exp_hit_logged[g_exp_n] = 0;
            g_exp_n++;
        }
    }
}

// Compare a GPU-bound group against the expected patched matrices.
uint32_t g_vspatched_logs = 0; // global one-shot cap (S1f)

void check_patched_gpu(const float *m16, uint32_t reg_base, uint64_t frame)
{
    if (g_vspatched_logs >= 12) {
        return;
    }
    for (uint32_t i = 0; i < g_exp_n; i++) {
        if (memcmp(m16, g_exp_mat[i], 64) == 0 || matrix_eq_T(m16, g_exp_mat[i])) {
            if (!g_exp_hit_logged[i]) {
                g_exp_hit_logged[i] = 1;
                g_vspatched_logs++;
                MC2VR_LOG("S1 vspatched: frame=%llu reg=c%u window=%s view=i%u "
                          "m%u — the PATCHED value reaches the GPU%s",
                          (unsigned long long)frame, reg_base, PATCH_NAMES[g_pstage],
                          g_pt_idx[i], g_pt_k[i],
                          matrix_eq_T(m16, g_exp_mat[i]) ? " (transposed)" : "");
            }
            return;
        }
    }
}

void patch_start_window(uint64_t frame)
{
    g_pstate = PatchState::Window;
    g_pleft = PATCH_WINDOW;
    patch_apply_targets();
    char tgt[96];
    int n = 0;
    const uint32_t shown = g_pt_n < 4 ? g_pt_n : 4;
    for (uint32_t i = 0; i < shown && n < (int)sizeof(tgt) - 16; i++) {
        n += _snprintf(tgt + n, sizeof(tgt) - n, " i%u", g_pt_idx[i]);
    }
    tgt[n] = '\0';
    if (g_pstage == 2) {
        MC2VR_LOG("S1 patch %s: %u ring records (stride 0x28, entry ptr +0x20; "
                  "first:%s) pos[0] += %g for %u frames — expected visual: "
                  "nudge #%d",
                  PATCH_NAMES[g_pstage], g_pt_n, tgt, (double)PATCH_DELTA,
                  (unsigned)PATCH_WINDOW, g_pstage + 1);
    } else {
        MC2VR_LOG("S1 patch %s: %u targets (anyLive=%u; first:%s) pos += %g for "
                  "%u frames — expected visual: nudge #%d",
                  PATCH_NAMES[g_pstage], g_pt_n, g_patch_any_live ? 1u : 0u,
                  tgt, (double)PATCH_DELTA, (unsigned)PATCH_WINDOW, g_pstage + 1);
    }
    (void)frame;
}

// One-shot ring-record dump for offline layout analysis: window C found no
// records with a walked ViewEntry* at +0x20 — dump raw records so the layout
// can be re-derived (S1d, run-3 lesson).
void dump_crecs()
{
    g_crec_dumped = true;
    if (!g_ctx_valid) {
        return;
    }
    const uint8_t *base = (const uint8_t *)(g_frame_ctx + CTX_RING_OFF);
    MC2VR_LOG("S1 crec: dumping 8 records (0x28 stride) at ctx+0x%08x — window "
              "C matched nothing (ViewEntry* at +0x20 suspect); offline: find "
              "the entry-pointer offset / record layout",
              (unsigned)CTX_RING_OFF);
    for (uint32_t r = 0; r < 8; r++) {
        const uint8_t *p = base + r * RING_REC_STRIDE;
        for (uint32_t off = 0; off < RING_REC_STRIDE; off += 16) {
            const uint32_t n = RING_REC_STRIDE - off < 16 ? RING_REC_STRIDE - off : 16;
            char hex[48];
            char *w = hex;
            for (uint32_t j = 0; j < n; j++) {
                *w++ = "0123456789abcdef"[p[off + j] >> 4];
                *w++ = "0123456789abcdef"[p[off + j] & 0xf];
            }
            *w = '\0';
            MC2VR_LOG("S1 crec rec%u +0x%02x: %s", r, off, hex);
        }
    }
}

void run_patch(uint64_t frame)
{
    switch (g_pstate) {
    case PatchState::Done:
        return;

    case PatchState::Window:
        patch_restore_targets();
        if (g_pstage == 2 || g_pstage == 6) {
            // Re-scan each frame: the ring records / ctx pos floats move as
            // the walk republishes.
            if (!build_patch_targets(frame)) {
                MC2VR_LOG("S1 patch %s: no targets this frame; ending window",
                          PATCH_NAMES[g_pstage]);
                g_pstate = PatchState::Gap;
                g_pgap = PATCH_GAP;
                g_pt_n = 0;
                g_exp_n = 0;
                return;
            }
        }
        if (--g_pleft == 0) {
            MC2VR_LOG("S1 patch %s ended: targets=%u — nudge observed? (yes -> "
                      "this channel feeds the draw camera)",
                      PATCH_NAMES[g_pstage], g_pt_n);
            g_pstate = PatchState::Gap;
            g_pgap = PATCH_GAP;
            g_pt_n = 0;
            g_exp_n = 0;
            return;
        }
        patch_apply_targets();
        return;

    case PatchState::Gap:
        if (--g_pgap > 0) {
            return;
        }
        g_pstage++;
        if (g_pstage > 7) {
            g_pstate = PatchState::Done;
            MC2VR_LOG("S1 patch: all windows A-H complete — residency_seen=%u",
                      g_residency_seen ? 1u : 0u);
            return;
        }
        g_pstate = PatchState::Idle;
        [[fallthrough]];

    case PatchState::Idle:
        // Both the initial start and the post-Gap stage advance land here:
        // build targets for the current stage and open the window. Gate on
        // gameplay-like frames (>= PATCH_GAMEPLAY_MIN_VIEWS) so the sequence
        // runs on the real camera, not a menu background (run-3 lesson).
        if (g_list_frame != frame || g_list_total < PATCH_GAMEPLAY_MIN_VIEWS) {
            return; // menu/cutscene/load phase — wait, no retry cost
        }
        if (++g_pstage_tries > PATCH_STAGE_TRIES) {
            g_pstage_tries = 0;
            g_pstage++;
            if (g_pstage > 7) {
                g_pstate = PatchState::Done;
                MC2VR_LOG("S1 patch: all stages starved — windows skipped");
                return;
            }
            MC2VR_LOG("S1 patch: stage %s starved after %u gameplay tries — "
                      "advancing to %s", PATCH_NAMES[g_pstage - 1],
                      (unsigned)PATCH_STAGE_TRIES, PATCH_NAMES[g_pstage]);
            return;
        }
        if (!build_patch_targets(frame)) {
            if (g_pstage == 2 && !g_crec_dumped) {
                dump_crecs();
            }
            return; // stage prerequisites missing — retry (budgeted)
        }
        g_pstage_tries = 0;
        patch_start_window(frame);
        return;
    }
}

// ---- S1c exfil --------------------------------------------------------------------

bool exfil_collect(const float *m16, uint32_t reg_base)
{
    if (g_exmat_n >= EXFIL_MATS) {
        return false;
    }
    // S1e: the first 48 uniques were mostly global-constant soup (run 4).
    // Collect low registers always; elsewhere only matrices whose
    // translation looks like a world position (camera-basis signature).
    if (reg_base >= 32) {
        const float tx = m16[12];
        if (!(tx > 5.0f || tx < -5.0f)) {
            return false;
        }
    }
    const uint32_t h = hash64((const uint8_t *)m16);
    for (uint32_t i = 0; i < g_exmat_n; i++) {
        if (g_exmats[i].hash == h) {
            return false; // duplicate content this frame
        }
    }
    g_exmats[g_exmat_n].hash = h;
    g_exmats[g_exmat_n].reg = reg_base;
    memcpy(g_exmats[g_exmat_n].raw, m16, 64);
    g_exmat_n++;
    return true;
}

void exfil_emit()
{
    MC2VR_LOG("S1 exfil: frame=%llu views=%u (entry m0/m1 + FOV from the "
              "selection-time snapshot; GPU matrices follow — offline: search "
              "derived relationships vs entry matrices)",
              (unsigned long long)g_ex_frame, g_ex_views_total);
    char hex[132];
    for (uint32_t i = 0; i < g_ex_entry_n; i++) {
        hex64(g_ex_entries[i].m0, hex, sizeof(hex));
        MC2VR_LOG("S1 exentry: i%u m0=%s", g_ex_entries[i].idx, hex);
        hex64(g_ex_entries[i].m1, hex, sizeof(hex));
        MC2VR_LOG("S1 exentry: i%u m1=%s", g_ex_entries[i].idx, hex);
        if (g_ex_entries[i].has_all) {
            for (uint32_t k = 2; k < VIEW_MATRIX_COUNT; k++) {
                hex64(g_ex_entries[i].mall + k * VIEW_MATRIX_STRIDE, hex, sizeof(hex));
                MC2VR_LOG("S1 exentry: i%u m%u=%s", g_ex_entries[i].idx, k, hex);
            }
        }
        if (i == 0) {
            MC2VR_LOG("S1 exfov: i%u fov(+0x2ec/2f4)=%08x/%08x near(+0x188)=%08x "
                      "pos7c4=(%g,%g,%g)",
                      g_ex_entries[i].idx, g_ex_fov[0], g_ex_fov[1], g_ex_fov[2],
                      (double)*(const float *)&g_ex_fov[3],
                      (double)*(const float *)&g_ex_fov[4],
                      (double)*(const float *)&g_ex_fov[5]);
        }
    }
    // S1f: the LIVE primary subobjects (via the block's resolved pointers at
    // +0x74) — emitted as exsub2/exsub3 so the analyzer's existing window
    // search covers them. Gameplay frames only (the first EXFIL_EARLY frames
    // are the menu-background phase).
    if (g_ctx_valid && g_exfil_done > EXFIL_EARLY) {
        const uint8_t *block0 = (const uint8_t *)(g_frame_ctx + CTX_BLOCK_OFF);
        for (uint32_t s = 0; s < 2; s++) {
            const uint8_t *live =
                *(const uint8_t *const *)(block0 + CTX_SUBPTR_OFF + s * 4);
            if (!live || (uintptr_t)live < 0x00100000 || (uintptr_t)live >= 0x80000000) {
                continue;
            }
            for (uint32_t off = 0; off < CTX_SUBOBJ_SCAN; off += 32) {
                const uint32_t n = CTX_SUBOBJ_SCAN - off < 32 ? CTX_SUBOBJ_SCAN - off : 32;
                char lhex[96];
                char *w = lhex;
                for (uint32_t j = 0; j < n; j++) {
                    *w++ = "0123456789abcdef"[live[off + j] >> 4];
                    *w++ = "0123456789abcdef"[live[off + j] & 0xf];
                }
                *w = '\0';
                MC2VR_LOG("S1 exsub%u +0x%03x: %s", s + 2, off, lhex);
            }
        }
        // The full low-register VS cache at emit time: offline diffing across
        // exfil frames finds the per-frame dynamic registers (the camera
        // candidates) by elimination — global constants are static.
        for (uint32_t r = 0; r < VS_PRIOR_REGS; r++) {
            if (!g_vs_valid[r]) {
                continue;
            }
            char chex[40];
            char *w = chex;
            for (uint32_t j = 0; j < 16; j++) {
                *w++ = "0123456789abcdef"[((const uint8_t *)g_vs_rows[r])[j] >> 4];
                *w++ = "0123456789abcdef"[((const uint8_t *)g_vs_rows[r])[j] & 0xf];
            }
            *w = '\0';
            MC2VR_LOG("S1 excache: c%u=%s", r, chex);
        }
    }
    // S1e: the ctx-block primary-subobject copies (what the VM consumer
    // receives inside the 0x680 element) — offline candidates for the
    // derived main-camera source.
    if (g_ctx_valid) {
        const uint8_t *block = (const uint8_t *)(g_frame_ctx + CTX_BLOCK_OFF);
        const uint8_t *subs = block + 0xEC;
        for (uint32_t s = 0; s < EXFIL_SUB_COUNT; s++) {
            const uint8_t *p = subs + s * EXFIL_SUB_SIZE;
            for (uint32_t off = 0; off < EXFIL_SUB_SIZE; off += 32) {
                const uint32_t n =
                    EXFIL_SUB_SIZE - off < 32 ? EXFIL_SUB_SIZE - off : 32;
                char shex[96];
                char *w = shex;
                for (uint32_t j = 0; j < n; j++) {
                    *w++ = "0123456789abcdef"[p[off + j] >> 4];
                    *w++ = "0123456789abcdef"[p[off + j] & 0xf];
                }
                *w = '\0';
                MC2VR_LOG("S1 exsub%u +0x%03x: %s", s, off, shex);
            }
        }
    }
    for (uint32_t i = 0; i < g_exmat_n; i++) {
        hex64(g_exmats[i].raw, hex, sizeof(hex));
        MC2VR_LOG("S1 exmat: reg=c%u m=%s", g_exmats[i].reg, hex);
    }
    g_exmat_n = 0;
    g_ex_pending = 0;
}

void exfil_maybe(uint64_t frame)
{
    // Emit the pending snapshot: its frame's render is complete by now (all
    // four bracket points fired). Entry matrices come from the selection-time
    // snapshot — the live view list has already moved to the current frame.
    if (g_ex_pending) {
        exfil_emit();
    }
    if (g_list_frame != frame || g_list_total == 0) {
        return; // no world views walked this frame
    }
    g_exfil_view_counter++;
    if (g_exfil_done >= EXFIL_FRAMES) {
        return;
    }
    if (g_exfil_done < EXFIL_EARLY || g_exfil_view_counter % EXFIL_STRIDE == 0) {
        g_exfil_done++;
        // Snapshot the walked views NOW: the walk of `frame` just ended, so
        // the entry values are final for this frame.
        g_ex_entry_n = 0;
        const uint32_t views = g_list_n < EXFIL_VIEWS ? g_list_n : EXFIL_VIEWS;
        for (uint32_t i = 0; i < views; i++) {
            const uint32_t idx = g_list[i].idx;
            if (idx >= VIEW_IDX_MAX) {
                continue;
            }
            const uint8_t *entry =
                (const uint8_t *)(VIEW_TABLE + (size_t)idx * VIEW_STRIDE);
            ExEntrySnap &snap = g_ex_entries[g_ex_entry_n];
            snap.idx = idx;
            memcpy(snap.m0, entry + VIEW_M0_OFF, 64);
            memcpy(snap.m1, entry + VIEW_M1_OFF, 64);
            // S1e: the first few walked views snapshot ALL nine matrices —
            // run 4 showed m[6] (not just m[0]) reaching the GPU exact, so
            // the offline search needs every matrix.
            if (i < EXFIL_FULL_VIEWS) {
                memcpy(snap.mall,
                       entry + VIEW_MATRIX_OFF,
                       VIEW_MATRIX_COUNT * VIEW_MATRIX_STRIDE);
                snap.has_all = 1;
            } else {
                snap.has_all = 0;
            }
            if (g_ex_entry_n == 0) {
                g_ex_fov[0] = *(const uint32_t *)(entry + 0x2ec);
                g_ex_fov[1] = *(const uint32_t *)(entry + 0x2f4);
                g_ex_fov[2] = *(const uint32_t *)(entry + 0x188);
                g_ex_fov[3] = *(const uint32_t *)(entry + VIEW_POS_OFF);
                g_ex_fov[4] = *(const uint32_t *)(entry + VIEW_POS_OFF + 4);
                g_ex_fov[5] = *(const uint32_t *)(entry + VIEW_POS_OFF + 8);
            }
            g_ex_entry_n++;
        }
        g_ex_views_total = g_list_total;
        g_exmat_n = 0;
        g_ex_frame = frame;
        g_ex_pending = 1;
    }
}

// ---- VS constant classification ----------------------------------------------------

void vs_count_result(const MatrixMatch &mm, uint32_t reg_base, uint64_t frame)
{
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
    check_residency(mm);

    if (mm.kind != MatrixMatch::Kind::None) {
        // Match-tag one-shot logging per unique (reg, tag).
        char tag[40];
        format_match_tag(mm, tag, sizeof(tag));
        bool logged = false;
        for (uint32_t i = 0; i < g_match_log_n; i++) {
            if (g_match_log[i].reg == reg_base &&
                strcmp(g_match_log[i].tag, tag) == 0) {
                logged = true;
                break;
            }
        }
        if (!logged && g_match_log_n < VS_MATCH_LOG_MAX) {
            g_match_log[g_match_log_n].reg = reg_base;
            strncpy(g_match_log[g_match_log_n].tag, tag,
                    sizeof(g_match_log[0].tag) - 1);
            g_match_log[g_match_log_n].tag[sizeof(g_match_log[0].tag) - 1] = '\0';
            g_match_log_n++;
            MC2VR_LOG("S1 vsmatch: frame=%llu reg=c%u tag=%s",
                      (unsigned long long)frame, reg_base, tag);
        }
    }
}

struct VsMemoEntry {
    uint32_t hash;
    uint8_t raw[64];
    MatrixMatch result;
    bool valid;
};
VsMemoEntry g_memo[VS_MEMO_SLOTS];

// Classify with content-hash memoization and budgets (S1c perf fix, S1d
// register priority). `priority` (low registers) bypasses the classification
// budget: the camera constants are few and memo-deduped, so they are always
// classified even in heavy scenes.
MatrixMatch vs_classify(const float *m16, bool priority)
{
    g_vs_classified++;
    const uint32_t h = hash64((const uint8_t *)m16);
    VsMemoEntry &e = g_memo[h % VS_MEMO_SLOTS];
    if (e.valid && e.hash == h && memcmp(e.raw, m16, 64) == 0) {
        g_vs_memo_hits++;
        return e.result;
    }
    if (!priority) {
        if (g_vs_cls_budget_left == 0) {
            g_vs_cls_skipped++;
            MatrixMatch none;
            return none;
        }
        g_vs_cls_budget_left--;
    }
    const bool region_scan = g_vs_region_budget_left > 0;
    if (region_scan) {
        g_vs_region_budget_left--;
    }
    g_vs_cls_new++;
    MatrixMatch mm = find_matrix(m16, region_scan);
    e.valid = true;
    e.hash = h;
    memcpy(e.raw, m16, 64);
    e.result = mm;
    return mm;
}

void vs_candidate(const float *m16, uint32_t reg_base, uint64_t frame, bool detail)
{
    if (g_ex_pending && frame == g_ex_frame) {
        exfil_collect(m16, reg_base);
    }
    // Register priority (S1d): low registers carry the camera/view constants —
    // classify them even when the per-frame budget is exhausted (they are few
    // and memo-deduped); high registers (per-object matrices) only get the
    // leftover budget.
    const MatrixMatch mm = vs_classify(m16, reg_base < VS_PRIOR_REGS);
    vs_count_result(mm, reg_base, frame);
    if (g_pstate == PatchState::Window && (g_pstage == 0 || g_pstage == 4 || g_pstage == 5)) {
        check_patched_gpu(m16, reg_base, frame); // S1f GPU proof
    }

    if (detail && g_vs_detail < VS_DETAIL_BURST) {
        g_vs_detail++;
        char tag[48];
        char hex[132];
        format_match_tag(mm, tag, sizeof(tag));
        hex64(m16, hex, sizeof(hex));
        MC2VR_LOG("S1 vsmat: frame=%llu reg=c%u tag=%s m=%s",
                  (unsigned long long)frame, reg_base, tag, hex);
    }
}

// ---- MidHook handlers -----------------------------------------------------------

// 0x004c99f9 — the `call 0x0050f660` itself: handler runs immediately before
// the VM'd packet interpreter, with the whole SubmitWorldPackets walk done.
void pre_vm_midhook(safetyhook::Context &)
{
    const uint64_t frame = hooks::frame_count();
    const uint32_t ca = read_counters_a();
    const uint32_t cb = *(volatile uint32_t *)MC2_QUEUE_COUNTERS_B;

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

    if (g_elem_dumps < ELEM_DUMP_FRAMES && s.views > 0) {
        g_elem_dumps++;
        scan_ring_world_elements(ca, cb);
    }

    exfil_maybe(frame);   // S1c: emit previous exfil block / select new frame
    log_view_list(frame); // S1.4: the walk just ended, the list is final
    run_patch(frame);     // S1c: patch windows A-E
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
            g_frame_ctx = 0;
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
    // S1 result: the engine never calls this. Counter kept as the per-run check.
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
        const MatrixMatch mm = find_matrix(m, true);
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
    if (frame != g_vs_frame_key) {
        g_vs_frame_key = frame;
        g_vs_cls_budget_left = VS_CLS_BUDGET;
        g_vs_region_budget_left = VS_REGION_BUDGET;
    }

    if (g_vs_calls <= 3) {
        MC2VR_LOG("S1 vs: first call #%llu: start=c%u count=%u frame=%llu",
                  (unsigned long long)g_vs_calls, start_register, vec4_count,
                  (unsigned long long)frame);
    }

    // Bulk path: 4-float-aligned groups inside one call (count >= 4 covers
    // whole-matrix uploads).
    if (vec4_count >= 4) {
        const uint32_t groups = vec4_count - 3 < VS_BULK_MAX_GROUPS
                                    ? vec4_count - 3
                                    : VS_BULK_MAX_GROUPS;
        const bool detail = g_vs_detail < VS_DETAIL_BURST;
        for (uint32_t i = 0; i < groups; i++) {
            vs_candidate(data + i * 4, start_register + i, frame, detail);
        }
    }

    // Row-cache path: matrices uploaded one float4 at a time. Cache rows;
    // when 4 consecutive registers are present, assemble and classify (the
    // transposed compare catches column-major uploads).
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
                vs_candidate(m16, r - 3, frame, g_vs_detail < VS_DETAIL_BURST);
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
    MC2VR_LOG("S1 bracket: frames=%llu changedPreRf=%llu changedRfCmd=%llu "
              "changedCmdEof=%llu none=%llu | A(lastPre)=%08x",
              (unsigned long long)g_full_brackets, (unsigned long long)g_adv_pipeline,
              (unsigned long long)g_adv_rf_to_cmd, (unsigned long long)g_adv_cmd_to_eof,
              (unsigned long long)g_adv_none, g_prev_a_pre);

    MC2VR_LOG("S1 vs: calls=%llu vec4s=%llu classified=%llu (new=%llu memoHit=%llu "
              "budgetSkipped=%llu) | entry=%llu entryT=%llu ctx=%llu sub=%llu "
              "none=%llu | residency=%s | patchStage=%s",
              (unsigned long long)g_vs_calls, (unsigned long long)g_vs_vec4s,
              (unsigned long long)g_vs_classified, (unsigned long long)g_vs_cls_new,
              (unsigned long long)g_vs_memo_hits, (unsigned long long)g_vs_cls_skipped,
              (unsigned long long)g_vs.entry, (unsigned long long)g_vs.entryT,
              (unsigned long long)g_vs.ctx, (unsigned long long)g_vs.sub,
              (unsigned long long)g_vs.none,
              g_residency_seen ? "PROVEN" : "not-yet",
              g_pstate == PatchState::Done ? "done" : PATCH_NAMES[g_pstage]);

    {
        // Top matching VS registers (bounded line, selection by hits).
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
              "satMax=%u sigs=%u | exfilDone=%u",
              (unsigned long long)g_list_frames, g_t2_min == 0xffffffff ? 0 : g_t2_min,
              g_t2_max, (unsigned long long)g_sat_frames, g_sat_max, g_sig_n,
              g_exfil_done);

    // Reset window aggregates. Process-lifetime one-shots are NOT reset.
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
    g_vs_cls_new = 0;
    g_vs_cls_skipped = 0;
    g_vs_memo_hits = 0;
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

// ---- install ----------------------------------------------------------------------

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
