#include "qpc.hpp"
#include "gpu_census.hpp"

#include <windows.h>
#include <d3d9.h>

#include <cstdint>
#include <cstring>

#include "log.hpp"
#include "vr/render_outputs.hpp"
#include "vr/view_rewrite.hpp"

namespace mc2vr::gpu_census {

namespace {

enum Cat : uint8_t { CAT_MAIN, CAT_ATLAS, CAT_SQUARE, CAT_OTHER, CAT_CARRIER, CAT_N };
const char *const kCat[CAT_N] = {"main", "atlas", "square", "other", "carrier"};
constexpr int kPasses = 3;  // 0 = between passes, 1 = left, 2 = right

constexpr int kMaxSeg = 96;
constexpr int kRing = 6;   // frames in flight before a record is reused
constexpr int kReadAge = 3;  // read a record this many frames after it closed

struct Frame {
    bool used = false;      // issued, results not yet read
    int n = 0;              // segments
    IDirect3DQuery9 *ts[kMaxSeg + 1] = {};
    IDirect3DQuery9 *disjoint = nullptr;
    IDirect3DQuery9 *freq = nullptr;
    uint8_t pass[kMaxSeg] = {};
    uint8_t cat[kMaxSeg] = {};
    uint32_t draws[kMaxSeg] = {};
    double cpu_ms[kMaxSeg] = {};
    uint64_t closed_at = 0;  // frame counter when closed
};

bool g_enabled = false;
IDirect3DDevice9 *g_device = nullptr;
Frame g_ring[kRing];
int g_cur = -1;           // open frame index
uint64_t g_frames = 0;    // frames opened
uint32_t g_pass = 0;
uint8_t g_cat = CAT_OTHER;
uint8_t g_cat_before_carrier = CAT_OTHER;
int64_t g_seg_t0 = 0;  // QPC ticks (qpc.hpp: exact on the single-precision render thread)

// window accumulators
uint64_t w_frames = 0, w_unread = 0, w_disjoint = 0;
double w_gpu[kPasses][CAT_N] = {}, w_cpu[kPasses][CAT_N] = {};
uint64_t w_draws[kPasses][CAT_N] = {};
double w_gpu_frame = 0.0;


IDirect3DQuery9 *create_query(D3DQUERYTYPE type)
{
    if (g_device == nullptr) return nullptr;
    IDirect3DQuery9 *q = nullptr;
    if (FAILED(g_device->CreateQuery(type, &q))) {
        return nullptr;
    }
    return q;
}

void issue(IDirect3DQuery9 *q, DWORD flags)
{
    if (q) q->Issue(flags);
}

bool get(IDirect3DQuery9 *q, void *out, DWORD size)
{
    return q && q->GetData(out, size, 0) == S_OK;
}

void release(IDirect3DQuery9 *&q)
{
    if (q) q->Release();
    q = nullptr;
}

bool ensure_queries(Frame &f)
{
    if (f.disjoint == nullptr) f.disjoint = create_query(D3DQUERYTYPE_TIMESTAMPDISJOINT);
    if (f.freq == nullptr) f.freq = create_query(D3DQUERYTYPE_TIMESTAMPFREQ);
    for (IDirect3DQuery9 *&q : f.ts) {
        if (q == nullptr) q = create_query(D3DQUERYTYPE_TIMESTAMP);
    }
    return f.disjoint && f.freq && f.ts[0];
}

// Start a segment in the open frame at the current pass/category.
void open_segment()
{
    if (g_cur < 0) return;
    Frame &f = g_ring[g_cur];
    const int64_t now = qpc::now();
    if (f.n > 0) f.cpu_ms[f.n - 1] += qpc::to_ms(now - g_seg_t0);
    g_seg_t0 = now;
    if (f.n >= kMaxSeg) return;  // overflow: the last segment absorbs the rest
    issue(f.ts[f.n], D3DISSUE_END);
    f.pass[f.n] = (uint8_t)g_pass;
    f.cat[f.n] = g_cat;
    f.draws[f.n] = 0;
    f.cpu_ms[f.n] = 0.0;
    f.n++;
}

void read_ready()
{
    for (Frame &f : g_ring) {
        if (!f.used || g_frames - f.closed_at < (uint64_t)kReadAge) continue;
        BOOL disjoint = TRUE;
        UINT64 freq = 0;
        UINT64 t[kMaxSeg + 1];
        bool ok = get(f.disjoint, &disjoint, sizeof disjoint) && get(f.freq, &freq, sizeof freq);
        for (int i = 0; ok && i <= f.n && i <= kMaxSeg; i++) {
            if (f.ts[i] == nullptr || !get(f.ts[i], &t[i], sizeof t[i])) ok = false;
        }
        f.used = false;
        if (!ok) {
            w_unread++;
            continue;
        }
        if (disjoint || freq == 0) {
            w_disjoint++;
            continue;
        }
        const double to_ms = 1000.0 / (double)freq;
        for (int i = 0; i < f.n; i++) {
            const double g = (double)(t[i + 1] - t[i]) * to_ms;
            w_gpu[f.pass[i]][f.cat[i]] += g;
            w_cpu[f.pass[i]][f.cat[i]] += f.cpu_ms[i];
            w_draws[f.pass[i]][f.cat[i]] += f.draws[i];
        }
        w_gpu_frame += (double)(t[f.n] - t[0]) * to_ms;
        w_frames++;
    }
}

void close_frame()
{
    if (g_cur < 0) return;
    Frame &f = g_ring[g_cur];
    if (f.n > 0) f.cpu_ms[f.n - 1] += qpc::ms_since(g_seg_t0);
    issue(f.ts[f.n < kMaxSeg ? f.n : kMaxSeg], D3DISSUE_END);
    issue(f.disjoint, D3DISSUE_END);
    f.closed_at = g_frames;
    f.used = f.n > 0;
    g_cur = -1;
}

void open_frame()
{
    read_ready();
    const int idx = (int)(g_frames % kRing);
    Frame &f = g_ring[idx];
    if (f.used) {  // never read back in time: drop it
        f.used = false;
        w_unread++;
    }
    g_frames++;
    if (!ensure_queries(f)) return;
    f.n = 0;
    issue(f.disjoint, D3DISSUE_BEGIN);
    issue(f.freq, D3DISSUE_END);
    g_cur = idx;
    g_seg_t0 = qpc::now();
    open_segment();
}

} // namespace

bool set_enabled(const char *value)
{
    if (strcmp(value, "on") == 0) g_enabled = true;
    else if (strcmp(value, "off") == 0) g_enabled = false;
    else return false;
    MC2VR_LOG("gpu: debug_gpu_census=%s", value);
    return true;
}

bool enabled()
{
    return g_enabled;
}

void on_pass(uint32_t pass)
{
    if (!g_enabled) return;
    if (g_device == nullptr) g_device = render_outputs::device();
    if (g_device == nullptr) return;
    g_pass = pass < kPasses ? pass : 0;
    if (pass == 1) {
        close_frame();
        open_frame();
        return;
    }
    open_segment();
}

void on_set_rt(uint32_t w, uint32_t h)
{
    if (!g_enabled || g_cur < 0 || g_cat == CAT_CARRIER) return;
    const uint8_t c = view_rewrite::is_main_size(w, h) ? CAT_MAIN
                      : (w == 1024 && h == 4096)      ? CAT_ATLAS
                      : (w == h)                      ? CAT_SQUARE
                                                      : CAT_OTHER;
    if (c == g_cat) return;  // same category: keep the segment
    g_cat = c;
    open_segment();
}

void on_draw()
{
    if (g_cur < 0) return;
    Frame &f = g_ring[g_cur];
    if (f.n > 0) f.draws[f.n - 1]++;
}

void carrier_begin()
{
    if (!g_enabled || g_cur < 0 || g_cat == CAT_CARRIER) return;
    g_cat_before_carrier = g_cat;
    g_cat = CAT_CARRIER;
    open_segment();
}

void carrier_end()
{
    if (!g_enabled || g_cur < 0 || g_cat != CAT_CARRIER) return;
    g_cat = g_cat_before_carrier;
    open_segment();
}

void on_reset()
{
    for (Frame &f : g_ring) {
        release(f.disjoint);
        release(f.freq);
        for (IDirect3DQuery9 *&q : f.ts) release(q);
        f.used = false;
        f.n = 0;
    }
    g_cur = -1;
    g_device = nullptr;
}

void report_window()
{
    if (!g_enabled) return;
    if (w_frames == 0) {
        MC2VR_LOG("gpu: no frames measured (unread=%llu disjoint=%llu)",
                  (unsigned long long)w_unread, (unsigned long long)w_disjoint);
    } else {
        const double n = (double)w_frames;
        MC2VR_LOG("gpu: %llu frames, GPU %.2f ms/frame (unread=%llu disjoint=%llu)",
                  (unsigned long long)w_frames, w_gpu_frame / n, (unsigned long long)w_unread,
                  (unsigned long long)w_disjoint);
        static const char *const kPass[kPasses] = {"between", "pass1", "pass2"};
        for (int p = 0; p < kPasses; p++) {
            for (int c = 0; c < CAT_N; c++) {
                if (w_draws[p][c] == 0 && w_gpu[p][c] < 0.005 * n) continue;
                MC2VR_LOG("gpu:   %-7s %-7s draws %6.1f/frame  CPU %5.2f ms  GPU %5.2f ms", kPass[p],
                          kCat[c], (double)w_draws[p][c] / n, w_cpu[p][c] / n, w_gpu[p][c] / n);
            }
        }
    }
    w_frames = w_unread = w_disjoint = 0;
    memset(w_gpu, 0, sizeof w_gpu);
    memset(w_cpu, 0, sizeof w_cpu);
    memset(w_draws, 0, sizeof w_draws);
    w_gpu_frame = 0.0;
}

} // namespace mc2vr::gpu_census
