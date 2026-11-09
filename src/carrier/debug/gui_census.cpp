#include "debug/gui_census.hpp"

#include <windows.h>
#include <d3d9.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "debug/build_census.hpp"
#include "game/addresses.h"
#include "game/render_cmd.hpp"
#include "game/render_shell.hpp"
#include "hooks/frame_tick.hpp"
#include "log.hpp"
#include "vr/render_outputs.hpp"

namespace mc2vr::gui_census {

namespace {

constexpr ULONGLONG kIntervalMs = 30000;
constexpr uint32_t kBefore = 160;     // non-GUI records logged before the first GUI record (reaches the world markers)
constexpr uint32_t kMaxRecords = 256;
constexpr uint32_t kMaxOps = 16;
constexpr uint32_t kMaxConsts = 1024;
constexpr uint32_t kMaxNamed = 12;

bool g_enabled = false;

struct Op {
    uint32_t d[5];  // opcode + first operands
};

struct Record {
    uint32_t rank;
    uint16_t index;
    int8_t gui;
    uint8_t view_type, view_context, rt_mask, stencil;
    uint16_t technique, material;
    bool has_stream;
    const void *stream;            // rec->cmdStream
    uint32_t emitter, emitter_above[3];  // Submit return address at build time, and further up
    bool stream_was_last;          // == g_LastExecuteStream at record start: ExecuteStream skips it
    uint8_t half_allowed, half_active;  // g_HalfResPassAllowed / Active at record start
    uint32_t stretches;            // StretchRects during the record
    void *stretch_src, *stretch_dst;  // the last one
    Op ops[kMaxOps];
    uint32_t op_count;
    uint32_t draws;
    void *rt0, *ds;  // at the first draw
    uint32_t rt_w, rt_h, rt_fmt;  // RT0's desc, when it is not a named surface
    uint8_t zen_mask, zwr_mask, blend_mask;
    uint16_t zfunc_mask;
};

struct Const {
    uint32_t record;  // index into g_records
    bool pixel;
    uint32_t start, count;
    float v[8];
};

struct Named {
    void *ptr;
    const char *name;
};

Record g_records[kMaxRecords];
uint32_t g_record_count = 0;
Const g_consts[kMaxConsts];
uint32_t g_const_count = 0;
uint32_t g_dropped = 0;
Named g_named[kMaxNamed];
uint32_t g_named_count = 0;
uint64_t g_frame = 0;
uint32_t g_first_gui_rank = 0, g_last_gui_rank = 0, g_list_length = 0;

// debug_gui_skip=cycle: skip one record class of the HUD segment at a time.
bool g_skip_cycle = false;
bool g_skip_each = false;     // debug_gui_skip=each: one 2D record after the composite at a time
bool g_skip_markers = false;  // debug_gui_skip=markers: records emitted under FUN_0060f610
constexpr uint32_t kMarkersLo = 0x0060f610u, kMarkersHi = 0x0060fe17u;  // Hud_DrawWorldMarkers
// debug_gui_skip=callers: phase k hides the records emitted under caller group k.
bool g_skip_callers = false;
struct CallerGroup {
    uint32_t lo, hi;
    const char *name;
};
const CallerGroup kCallerGroups[] = {
    {0x0061e5d0u, 0x0061ebefu, "FUN_0061e5d0"},
    {0x0061f430u, 0x0061f620u, "FUN_0061f430"},
    {0x00625620u, 0x00626464u, "FUN_00625620"},
    {0x0046a699u, 0x0046a69au, "the VM-protected caller (return 0x0046a699)"},
};
constexpr uint32_t kCallerPhases = 1 + sizeof(kCallerGroups) / sizeof(kCallerGroups[0]);

bool emitted_under(uint16_t index, uint32_t lo, uint32_t hi)
{
    uint32_t above[3];
    const uint32_t ret = build_census::record_emitter(game::g_PrimitiveBase::get(), index, above);
    if (ret >= lo && ret < hi) return true;
    for (uint32_t a : above) {
        if (a >= lo && a < hi) return true;
    }
    return false;
}
uint32_t g_each_count = 0;    // 2D records after the composite, this pass
bool g_each_after_composite = false;
constexpr uint32_t kEachPhases = 13;  // 0 = reference, 1..12 = the k-th record
constexpr ULONGLONG kEachPhaseMs = 5000;
uint32_t g_skip_phase = 0;
ULONGLONG g_skip_next_ms = 0;
bool g_skip_located = false;  // this frame's GUI ranks are known
uint32_t g_skip_rank = 0;
constexpr ULONGLONG kSkipPhaseMs = 10000;
constexpr uint32_t kSkipPhases = 6;
const char *const kSkipNames[kSkipPhases] = {
    "nothing (reference)",
    "the Scaleform (GUI) records",
    "non-GUI technique-73 view-type-8 records before the GUI block",
    "non-GUI technique-73 view-type-5 records before the GUI block",
    "non-GUI records after the GUI block",
    "non-GUI records before the GUI block that are not technique 73",
};

bool g_capturing = false;
uint32_t g_rank = 0, g_from_rank = 0;
Record *g_cur = nullptr;  // the record being drawn, when logged
std::atomic<bool> g_ready{false};
ULONGLONG g_next_ms = 0;
// Half-res-pass trigger: a frame whose pass 1 ran the half-res pass makes the
// next frame a capture (rate-limited), whatever the 30 s timer says.
constexpr ULONGLONG kHalfIntervalMs = 10000;
ULONGLONG g_next_half_ms = 0;
bool g_half_this_frame = false, g_half_last_frame = false;
bool g_capture_is_half = false;

void *g_rt0 = nullptr, *g_ds = nullptr;
DWORD g_zen = 0, g_zwr = 0, g_zfunc = 0, g_blend = 0;

void add_named(void *ptr, const char *name)
{
    if (ptr != nullptr && g_named_count < kMaxNamed) g_named[g_named_count++] = {ptr, name};
}

const char *name_of(void *ptr, char *buf, size_t n)
{
    if (ptr == nullptr) return "-";
    for (uint32_t i = 0; i < g_named_count; i++) {
        if (g_named[i].ptr == ptr) return g_named[i].name;
    }
    std::snprintf(buf, n, "%p", ptr);
    return buf;
}

// Walks the published list: rank of the first GUI record and the list length.
// False when the GUI marks are unavailable or there is no GUI record.
bool locate_gui()
{
    using namespace game;
    const PgPrimitive *const base = g_PrimitiveBase::get();
    uint32_t idx = g_PrimitiveHead::get();
    bool found = false;
    uint32_t rank = 0;
    for (; idx != kPrimListEnd && idx < kPrimitivesPerBuffer && rank < kPrimitivesPerBuffer; rank++) {
        const int gui = build_census::is_gui_record(base, idx);
        if (gui < 0) return false;
        if (gui == 1) {
            if (!found) g_first_gui_rank = rank;
            found = true;
            g_last_gui_rank = rank;
        }
        idx = g_PrimitiveSortKeys::at(idx).next;
    }
    g_list_length = rank;
    return found;
}

void begin_capture()
{
    IDirect3DDevice9 *const dev = render_outputs::device();
    if (dev == nullptr || !locate_gui()) {
        return;
    }
    g_from_rank = g_first_gui_rank > kBefore ? g_first_gui_rank - kBefore : 0;
    g_record_count = g_const_count = g_dropped = 0;
    g_rank = 0;
    g_cur = nullptr;
    g_frame = hooks::frame_count();
    g_named_count = 0;
    static const char *const kSlots[MC2_RS_RT_TABLE_SLOTS] = {
        "main", "autoDepth", "frontRef", "depthCopy", "sceneDepth", "front", "half3ea8", "half3eac", "half3eb0"};
    for (uint32_t i = 0; i < MC2_RS_RT_TABLE_SLOTS; i++) {
        add_named(*reinterpret_cast<void **>(reinterpret_cast<uint8_t *>(game::render_shell()) + MC2_RS_RT_TABLE_OFF +
                                             i * 4u),
                  kSlots[i]);
    }
    IDirect3DSurface9 *s = nullptr;
    g_rt0 = g_ds = nullptr;
    if (SUCCEEDED(dev->GetRenderTarget(0, &s)) && s != nullptr) { g_rt0 = s; s->Release(); }
    s = nullptr;
    if (SUCCEEDED(dev->GetDepthStencilSurface(&s)) && s != nullptr) { g_ds = s; s->Release(); }
    dev->GetRenderState(D3DRS_ZENABLE, &g_zen);
    dev->GetRenderState(D3DRS_ZWRITEENABLE, &g_zwr);
    dev->GetRenderState(D3DRS_ZFUNC, &g_zfunc);
    dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &g_blend);
    g_capturing = true;
}

const char *kZFunc[9] = {"?", "NEVER", "LESS", "EQUAL", "LESSEQUAL", "GREATER", "NOTEQUAL", "GREATEREQUAL", "ALWAYS"};

void bits(uint32_t mask, uint32_t n, char *out, size_t cap)
{
    size_t len = 0;
    out[0] = '\0';
    for (uint32_t b = 0; b < n && len + 4 < cap; b++) {
        if (mask & (1u << b)) len += std::snprintf(out + len, cap - len, "%s%u", len ? "," : "", b);
    }
}

void print_capture()
{
    MC2VR_LOG("gui: capture frame %llu pass 1 (%s): %u records logged%s", (unsigned long long)g_frame,
              g_capture_is_half ? "the previous frame ran the half-res pass" : "30 s timer", g_record_count,
              g_dropped ? ", some dropped" : "");
    char line[512], a[24], b[24], zf[96], ze[16], zw[16], bl[16];
    uint32_t c = 0;
    for (uint32_t i = 0; i < g_record_count; i++) {
        const Record &r = g_records[i];
        size_t len = 0;
        for (uint32_t k = 0; k < r.op_count && len + 64 < sizeof line; k++) {
            const Op &o = r.ops[k];
            len += std::snprintf(line + len, sizeof line - len, "%s%u(%x,%x,%x,%x)", k ? " " : "", o.d[0], o.d[1],
                                 o.d[2], o.d[3], o.d[4]);
        }
        if (len == 0) std::snprintf(line, sizeof line, "%s", r.has_stream ? "(none)" : "(no stream)");
        zf[0] = '\0';
        size_t zl = 0;
        for (uint32_t f = 1; f <= 8; f++) {
            if (r.zfunc_mask & (1u << f)) zl += std::snprintf(zf + zl, sizeof zf - zl, "%s%s", zl ? "," : "", kZFunc[f]);
        }
        bits(r.zen_mask, 2, ze, sizeof ze);
        bits(r.zwr_mask, 2, zw, sizeof zw);
        bits(r.blend_mask, 2, bl, sizeof bl);
        char size[32] = "";
        if (r.rt_w != 0) std::snprintf(size, sizeof size, "(%ux%u fmt%u)", r.rt_w, r.rt_h, r.rt_fmt);
        MC2VR_LOG("gui: r%u idx=%u %s vt=%u vc=%u tech=%u mat=%u rtmask=%x stencil=%u draws=%u RT0=%s%s DS=%s zen={%s} "
                  "zwr={%s} zfunc={%s} blend={%s}",
                  r.rank, r.index, r.gui == 1 ? "GUI" : r.gui == 0 ? "---" : "?", r.view_type, r.view_context,
                  r.technique, r.material, r.rt_mask, r.stencil, r.draws, name_of(r.rt0, a, sizeof a), size,
                  name_of(r.ds, b, sizeof b), ze, zw, zf, bl);
        MC2VR_LOG("gui:     emitter %08x <- %08x <- %08x <- %08x | stream %p%s half allowed=%u active=%u", r.emitter,
                  r.emitter_above[0], r.emitter_above[1], r.emitter_above[2],
                  r.stream,
                  r.stream_was_last ? " (== last executed: skipped)" : "", r.half_allowed, r.half_active);
        MC2VR_LOG("gui:     ops %s", line);
        if (r.stretches != 0) {
            char s1[24], s2[24];
            MC2VR_LOG("gui:     %u StretchRect(s), last %s -> %s", r.stretches, name_of(r.stretch_src, s1, sizeof s1),
                      name_of(r.stretch_dst, s2, sizeof s2));
        }
        for (; c < g_const_count && g_consts[c].record == i; c++) {
            const Const &k = g_consts[c];
            MC2VR_LOG("gui:     %s c%u x%u: %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g", k.pixel ? "PS" : "VS", k.start,
                      k.count, k.v[0], k.v[1], k.v[2], k.v[3], k.v[4], k.v[5], k.v[6], k.v[7]);
        }
    }
}

} // namespace

bool set_enabled(const char *value)
{
    if (std::strcmp(value, "on") == 0) g_enabled = true;
    else if (std::strcmp(value, "off") == 0) g_enabled = false;
    else return false;
    if (g_enabled) build_census::require_gui_marks();
    MC2VR_LOG("gui: debug_gui_census=%s", value);
    return true;
}

bool enabled()
{
    return g_enabled;
}

void on_mirror_refreshed(IDirect3DDevice9 *device, IDirect3DSurface9 *mirror)
{
    if (!g_enabled || !(g_skip_cycle || g_skip_each || g_skip_callers) || device == nullptr || mirror == nullptr) return;
    D3DSURFACE_DESC d = {};
    if (FAILED(mirror->GetDesc(&d))) return;
    const LONG size = (LONG)d.Height / 20, gap = size / 2;
    for (uint32_t i = 0; i < g_skip_phase; i++) {
        const RECT r = {gap + (LONG)i * (size + gap), gap, gap + (LONG)i * (size + gap) + size, gap + size};
        device->ColorFill(mirror, &r, D3DCOLOR_XRGB(255, 255, 255));
    }
}

bool set_skip(const char *value)
{
    g_skip_cycle = g_skip_each = g_skip_markers = g_skip_callers = false;
    if (std::strcmp(value, "cycle") == 0) g_skip_cycle = true;
    else if (std::strcmp(value, "each") == 0) g_skip_each = true;
    else if (std::strcmp(value, "markers") == 0) g_skip_markers = true;
    else if (std::strcmp(value, "callers") == 0) g_skip_callers = true;
    else if (std::strcmp(value, "off") == 0) {}
    else return false;
    MC2VR_LOG("gui: debug_gui_skip=%s%s", value,
              (g_skip_cycle || g_skip_each || g_skip_markers || g_skip_callers) && !g_enabled
                  ? " (needs debug_gui_census=on)"
                  : "");
    return true;
}

void on_pass(uint32_t pass)
{
    if (!g_enabled) return;
    if ((g_skip_cycle || g_skip_each || g_skip_callers) && (pass == 1 || pass == 2)) {
        if (pass == 1) {
            const ULONGLONG now = GetTickCount64();
            if (now >= g_skip_next_ms) {
                const uint32_t phases = g_skip_each ? kEachPhases : g_skip_callers ? kCallerPhases : kSkipPhases;
                if (g_skip_next_ms != 0) g_skip_phase = (g_skip_phase + 1) % phases;
                g_skip_next_ms = now + (g_skip_each ? kEachPhaseMs : kSkipPhaseMs);
                if (g_skip_callers) {
                    MC2VR_LOG("gui: skip phase %u: skipping %s", g_skip_phase,
                              g_skip_phase == 0 ? "nothing (reference)" : kCallerGroups[g_skip_phase - 1].name);
                } else if (g_skip_each) {
                    MC2VR_LOG("gui: skip phase %u: skipping %s", g_skip_phase,
                              g_skip_phase == 0 ? "nothing (reference)" : "one 2D record after the composite");
                } else {
                    MC2VR_LOG("gui: skip phase %u: skipping %s", g_skip_phase, kSkipNames[g_skip_phase]);
                }
            }
            g_skip_located = locate_gui();
        }
        g_skip_rank = 0;
        g_each_count = 0;
        g_each_after_composite = false;
    }
    if (pass == 1) {
        g_half_last_frame = g_half_this_frame;
        g_half_this_frame = false;
    }
    if (pass == 1 && !g_capturing && !g_ready.load(std::memory_order_acquire)) {
        const ULONGLONG now = GetTickCount64();
        if (g_half_last_frame && now >= g_next_half_ms) {
            g_next_half_ms = now + kHalfIntervalMs;
            g_capture_is_half = true;
            begin_capture();
        } else if (now >= g_next_ms) {
            g_next_ms = now + kIntervalMs;
            g_capture_is_half = false;
            begin_capture();
        }
    } else if (pass != 1 && g_capturing) {
        g_capturing = false;
        g_cur = nullptr;
        g_ready.store(true, std::memory_order_release);
    }
}

bool skip_record(uint16_t index, const game::PgPrimitive *rec)
{
    if (g_skip_markers) {
        return emitted_under(index, kMarkersLo, kMarkersHi);
    }
    if (g_skip_callers) {
        if (g_skip_phase == 0) return false;
        const CallerGroup &g = kCallerGroups[g_skip_phase - 1];
        return emitted_under(index, g.lo, g.hi);
    }
    if (g_skip_each) {
        // The composite is the technique-79 record; the k-th non-GUI
        // technique-73 record after it is skipped in phase k.
        if (rec->techniqueIdx == 79) {
            g_each_after_composite = true;
            return false;
        }
        if (!g_each_after_composite || rec->techniqueIdx != 73 ||
            build_census::is_gui_record(game::g_PrimitiveBase::get(), index) == 1) {
            return false;
        }
        return ++g_each_count == g_skip_phase;
    }
    if (!g_skip_cycle || !g_skip_located || g_skip_phase == 0) return false;
    const uint32_t rank = g_skip_rank++;
    const uint32_t from = g_first_gui_rank > kBefore ? g_first_gui_rank - kBefore : 0;
    if (rank < from) return false;
    const bool gui = build_census::is_gui_record(game::g_PrimitiveBase::get(), index) == 1;
    const bool before = rank < g_first_gui_rank, after = rank > g_last_gui_rank;
    switch (g_skip_phase) {
    case 1: return gui;
    case 2: return !gui && before && rec->techniqueIdx == 73 && rec->viewType == 8;
    case 3: return !gui && before && rec->techniqueIdx == 73 && rec->viewType == 5;
    case 4: return !gui && after;
    case 5: return !gui && before && rec->techniqueIdx != 73;
    default: return false;
    }
}

void on_record(uint16_t index, const game::PgPrimitive *rec)
{
    if (g_enabled && game::g_HalfResPassActive::get() != 0) g_half_this_frame = true;
    if (!g_capturing) return;
    const uint32_t rank = g_rank++;
    g_cur = nullptr;
    if (rank < g_from_rank) return;
    if (g_record_count >= kMaxRecords) {
        g_dropped++;
        return;
    }
    Record &r = g_records[g_record_count++];
    std::memset(&r, 0, sizeof r);
    r.rank = rank;
    r.index = index;
    r.gui = (int8_t)build_census::is_gui_record(game::g_PrimitiveBase::get(), index);
    r.view_type = rec->viewType;
    r.view_context = rec->viewContextIdx;
    r.rt_mask = rec->rtWriteMask;
    r.stencil = rec->stencilMode;
    r.technique = rec->techniqueIdx;
    r.material = rec->materialIdx;
    r.has_stream = rec->cmdStream != nullptr;
    r.stream = rec->cmdStream;
    r.emitter = build_census::record_emitter(game::g_PrimitiveBase::get(), index, r.emitter_above);
    r.stream_was_last = rec->cmdStream != nullptr && rec->cmdStream == game::g_LastExecuteStream::get();
    r.half_allowed = game::g_HalfResPassAllowed::get();
    r.half_active = game::g_HalfResPassActive::get();
    g_cur = &r;
}

void on_command(const uint32_t *cmd)
{
    if (g_cur == nullptr || g_cur->op_count >= kMaxOps) return;
    Op &o = g_cur->ops[g_cur->op_count++];
    o.d[0] = cmd[0];
    if (!IsBadReadPtr(cmd, sizeof o.d)) {  // operands may run past a short final command
        for (int i = 1; i < 5; i++) o.d[i] = cmd[i];
    }
}

void on_stretch_rect(IDirect3DSurface9 *src, IDirect3DSurface9 *dst)
{
    if (g_cur == nullptr) return;
    g_cur->stretches++;
    g_cur->stretch_src = src;
    g_cur->stretch_dst = dst;
}

void on_set_render_target(DWORD index, IDirect3DSurface9 *surface)
{
    if (g_capturing && index == 0) g_rt0 = surface;
}

void on_set_depth(IDirect3DSurface9 *surface)
{
    if (g_capturing) g_ds = surface;
}

void on_shader_constants(bool pixel, UINT start, const float *data, UINT count)
{
    if (g_cur == nullptr) return;
    if (g_const_count >= kMaxConsts) {
        g_dropped++;
        return;
    }
    Const &k = g_consts[g_const_count++];
    std::memset(&k, 0, sizeof k);
    k.record = (uint32_t)(g_cur - g_records);
    k.pixel = pixel;
    k.start = start;
    k.count = count;
    if (data != nullptr) {
        const UINT n = count * 4 < 8 ? count * 4 : 8;
        for (UINT i = 0; i < n; i++) k.v[i] = data[i];
    }
}

void on_render_state(D3DRENDERSTATETYPE state, DWORD value)
{
    if (!g_capturing) return;
    switch (state) {
    case D3DRS_ZENABLE: g_zen = value; break;
    case D3DRS_ZWRITEENABLE: g_zwr = value; break;
    case D3DRS_ZFUNC: g_zfunc = value; break;
    case D3DRS_ALPHABLENDENABLE: g_blend = value; break;
    default: break;
    }
}

void on_draw()
{
    if (g_cur == nullptr) return;
    // Draws after the last record (the frame-end view-type epilogue) count
    // into the last record.
    Record &r = *g_cur;
    if (r.draws++ == 0) {
        r.rt0 = g_rt0;
        r.ds = g_ds;
        char unused[24];
        D3DSURFACE_DESC d = {};
        if (g_rt0 != nullptr && name_of(g_rt0, unused, sizeof unused) == unused &&
            SUCCEEDED(static_cast<IDirect3DSurface9 *>(g_rt0)->GetDesc(&d))) {
            r.rt_w = d.Width;
            r.rt_h = d.Height;
            r.rt_fmt = (uint32_t)d.Format;
        }
    }
    r.zen_mask |= (uint8_t)(1u << (g_zen ? 1 : 0));
    r.zwr_mask |= (uint8_t)(1u << (g_zwr ? 1 : 0));
    r.blend_mask |= (uint8_t)(1u << (g_blend ? 1 : 0));
    if (g_zfunc >= 1 && g_zfunc <= 8) r.zfunc_mask |= (uint16_t)(1u << g_zfunc);
}

void report_window()
{
    if (!g_enabled || !g_ready.load(std::memory_order_acquire)) return;
    print_capture();
    g_ready.store(false, std::memory_order_release);
}

} // namespace mc2vr::gui_census
