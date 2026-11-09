#include "ps_census.hpp"

#include <windows.h>
#include <d3d9.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "log.hpp"
#include "vr/render_outputs.hpp"
#include "vr/view_rewrite.hpp"

namespace mc2vr::ps_census {

namespace {

constexpr uint32_t kSampleEvery = 8;
constexpr int kSeg = 4096;
constexpr int kPasses = 3;
enum Cat : uint8_t { CAT_MAIN, CAT_ATLAS, CAT_SQUARE, CAT_OTHER, CAT_N };
const char *const kCat[CAT_N] = {"main", "atlas", "square", "other"};

struct Rec {
    bool pending = false;
    int n = 0;
    std::vector<IDirect3DQuery9 *> ts, occ;  // grown on demand (queries kept for reuse)
    IDirect3DQuery9 *disjoint = nullptr, *freq = nullptr;
    IDirect3DPixelShader9 *ps[kSeg];
    uint8_t pass[kSeg];
    uint8_t cat[kSeg];
    uint16_t draws[kSeg];
};

bool g_enabled = false;
IDirect3DDevice9 *g_device = nullptr;
Rec g_rec[2];
int g_cur = -1;
uint64_t g_frames = 0;
uint32_t g_pass = 0;
uint8_t g_cat = CAT_OTHER;
IDirect3DPixelShader9 *g_ps = nullptr;

// ---- per-shader aggregation ----------------------------------------------------
struct Shader {
    IDirect3DPixelShader9 *obj;
    uint32_t hash, size;
    double gpu[kPasses];
    uint64_t pix[kPasses];
    uint64_t draws[kPasses];
    uint8_t cat;
};
constexpr int kShaders = 2048;
Shader g_sh[kShaders];
int g_sh_n = 0;
uint64_t w_frames = 0, w_unread = 0, w_overflow = 0;
double w_gpu_total[kPasses] = {};
uint64_t w_pix_total[kPasses][CAT_N] = {};

IDirect3DQuery9 *create_query(D3DQUERYTYPE type)
{
    IDirect3DQuery9 *q = nullptr;
    if (g_device == nullptr ||
        FAILED(g_device->CreateQuery(type, &q))) {
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

uint32_t fnv1a(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

// Lookup; with `create`, register (and hash) a newly seen shader — only while
// it is bound (SetPixelShader), when the object is certainly alive.
Shader *shader_for(IDirect3DPixelShader9 *obj, bool create)
{
    for (int i = 0; i < g_sh_n; i++) {
        if (g_sh[i].obj == obj) return &g_sh[i];
    }
    if (!create || g_sh_n >= kShaders) return nullptr;
    Shader &s = g_sh[g_sh_n++];
    memset(&s, 0, sizeof s);
    s.obj = obj;
    if (obj) {
        UINT size = 0;
        IDirect3DPixelShader9 *ps = obj;
        if (SUCCEEDED(ps->GetFunction(nullptr, &size)) && size > 0 && size < (1u << 20)) {
            std::vector<uint8_t> buf(size);
            if (SUCCEEDED(ps->GetFunction(buf.data(), &size))) {
                s.hash = fnv1a(buf.data(), size);
                s.size = size;
            }
        }
    }
    return &s;
}

// Query slot i of the open record (created on first use).
bool ensure(Rec &r, int i)
{
    while ((int)r.ts.size() <= i + 1) r.ts.push_back(create_query(D3DQUERYTYPE_TIMESTAMP));
    while ((int)r.occ.size() <= i) r.occ.push_back(create_query(D3DQUERYTYPE_OCCLUSION));
    return r.ts[i] && r.ts[i + 1] && r.occ[i];
}

void close_segment(Rec &r)
{
    if (r.n > 0) issue(r.occ[r.n - 1], D3DISSUE_END);
}

void open_segment()
{
    if (g_cur < 0) return;
    Rec &r = g_rec[g_cur];
    if (r.n >= kSeg || !ensure(r, r.n)) {
        w_overflow++;
        return;  // the last segment absorbs the rest
    }
    close_segment(r);
    issue(r.ts[r.n], D3DISSUE_END);
    issue(r.occ[r.n], D3DISSUE_BEGIN);
    r.ps[r.n] = g_ps;
    r.pass[r.n] = (uint8_t)g_pass;
    r.cat[r.n] = g_cat;
    r.draws[r.n] = 0;
    r.n++;
}

void read_rec(Rec &r)
{
    if (!r.pending) return;
    r.pending = false;
    BOOL disjoint = TRUE;
    UINT64 freq = 0;
    if (!get(r.disjoint, &disjoint, sizeof disjoint) || !get(r.freq, &freq, sizeof freq) ||
        disjoint || freq == 0) {
        w_unread++;
        return;
    }
    std::vector<UINT64> t(r.n + 1);
    std::vector<DWORD> px(r.n);
    for (int i = 0; i <= r.n; i++) {
        if (!get(r.ts[i], &t[i], sizeof t[i])) { w_unread++; return; }
    }
    for (int i = 0; i < r.n; i++) {
        if (!get(r.occ[i], &px[i], sizeof px[i])) { w_unread++; return; }
    }
    const double to_ms = 1000.0 / (double)freq;
    for (int i = 0; i < r.n; i++) {
        Shader *s = shader_for(r.ps[i], false);
        const double g = (double)(t[i + 1] - t[i]) * to_ms;
        w_gpu_total[r.pass[i]] += g;
        w_pix_total[r.pass[i]][r.cat[i]] += px[i];
        if (!s) continue;
        s->gpu[r.pass[i]] += g;
        s->pix[r.pass[i]] += px[i];
        s->draws[r.pass[i]] += r.draws[i];
        s->cat = r.cat[i];
    }
    w_frames++;
}

void close_frame()
{
    if (g_cur < 0) return;
    Rec &r = g_rec[g_cur];
    close_segment(r);
    if (r.n > 0 && (int)r.ts.size() > r.n) issue(r.ts[r.n], D3DISSUE_END);
    issue(r.disjoint, D3DISSUE_END);
    r.pending = r.n > 0;
    g_cur = -1;
}

void open_frame()
{
    const int idx = (int)((g_frames / kSampleEvery) % 2);
    Rec &r = g_rec[idx];
    read_rec(g_rec[idx ^ 1]);  // the previous sample, kSampleEvery frames old
    read_rec(r);
    if (r.disjoint == nullptr) r.disjoint = create_query(D3DQUERYTYPE_TIMESTAMPDISJOINT);
    if (r.freq == nullptr) r.freq = create_query(D3DQUERYTYPE_TIMESTAMPFREQ);
    if (!r.disjoint || !r.freq) return;
    r.n = 0;
    issue(r.disjoint, D3DISSUE_BEGIN);
    issue(r.freq, D3DISSUE_END);
    g_cur = idx;
    shader_for(g_ps, true);
    open_segment();
}

} // namespace

bool set_enabled(const char *value)
{
    if (strcmp(value, "on") == 0) g_enabled = true;
    else if (strcmp(value, "off") == 0) g_enabled = false;
    else return false;
    MC2VR_LOG("ps: debug_ps_census=%s", value);
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
        if (g_frames++ % kSampleEvery == 0) open_frame();
        return;
    }
    open_segment();
}

void on_set_rt(uint32_t w, uint32_t h)
{
    if (!g_enabled) return;
    const uint8_t c = view_rewrite::is_main_size(w, h) ? CAT_MAIN
                      : (w == 1024 && h == 4096)      ? CAT_ATLAS
                      : (w == h)                      ? CAT_SQUARE
                                                      : CAT_OTHER;
    if (c == g_cat) return;
    g_cat = c;
    open_segment();
}

void on_set_ps(IDirect3DPixelShader9 *ps)
{
    if (!g_enabled || ps == g_ps) return;
    g_ps = ps;
    if (g_cur >= 0) shader_for(ps, true);
    open_segment();
}

void on_draw()
{
    if (g_cur < 0) return;
    Rec &r = g_rec[g_cur];
    if (r.n > 0 && r.draws[r.n - 1] < 0xffff) r.draws[r.n - 1]++;
}

void on_reset()
{
    for (Rec &r : g_rec) {
        for (IDirect3DQuery9 *&q : r.ts) release(q);
        for (IDirect3DQuery9 *&q : r.occ) release(q);
        r.ts.clear();
        r.occ.clear();
        release(r.disjoint);
        release(r.freq);
        r.pending = false;
        r.n = 0;
    }
    g_cur = -1;
    g_device = nullptr;
    g_ps = nullptr;
    g_sh_n = 0;  // shader objects are recreated after Reset
}

void report_window()
{
    if (!g_enabled) return;
    if (w_frames == 0) {
        MC2VR_LOG("ps: no frames measured (unread=%llu)", (unsigned long long)w_unread);
        return;
    }
    const double n = (double)w_frames;
    MC2VR_LOG("ps: %llu sampled frames (1 in %u) unread=%llu overflow=%llu | GPU ms/frame "
              "between %.2f pass1 %.2f pass2 %.2f",
              (unsigned long long)w_frames, kSampleEvery, (unsigned long long)w_unread,
              (unsigned long long)w_overflow, w_gpu_total[0] / n, w_gpu_total[1] / n,
              w_gpu_total[2] / n);
    for (int p = 1; p < kPasses; p++) {
        MC2VR_LOG("ps:   pass%d Mpix/frame main %.2f atlas %.2f square %.2f other %.2f", p,
                  w_pix_total[p][CAT_MAIN] / n / 1e6, w_pix_total[p][CAT_ATLAS] / n / 1e6,
                  w_pix_total[p][CAT_SQUARE] / n / 1e6, w_pix_total[p][CAT_OTHER] / n / 1e6);
    }
    // top shaders by GPU time (both eye passes)
    bool used[kShaders] = {};
    for (int k = 0; k < 20; k++) {
        int best = -1;
        double bv = 0.0;
        for (int i = 0; i < g_sh_n; i++) {
            const double v = g_sh[i].gpu[1] + g_sh[i].gpu[2];
            if (!used[i] && v > bv) { bv = v; best = i; }
        }
        if (best < 0 || bv / n < 0.01) break;
        used[best] = true;
        const Shader &s = g_sh[best];
        const double pix = (double)(s.pix[1] + s.pix[2]);
        MC2VR_LOG("ps:   %08x %5uB %-6s GPU p1 %.3f p2 %.3f ms | Mpix p1 %.2f p2 %.2f | "
                  "draws %.1f | %.2f ns/pix",
                  s.hash, s.size, kCat[s.cat], s.gpu[1] / n, s.gpu[2] / n, s.pix[1] / n / 1e6,
                  s.pix[2] / n / 1e6, (double)(s.draws[1] + s.draws[2]) / n,
                  pix > 0 ? bv * 1e6 / pix : 0.0);
    }
    for (int i = 0; i < g_sh_n; i++) {
        memset(g_sh[i].gpu, 0, sizeof g_sh[i].gpu);
        memset(g_sh[i].pix, 0, sizeof g_sh[i].pix);
        memset(g_sh[i].draws, 0, sizeof g_sh[i].draws);
    }
    w_frames = w_unread = w_overflow = 0;
    memset(w_gpu_total, 0, sizeof w_gpu_total);
    memset(w_pix_total, 0, sizeof w_pix_total);
}

} // namespace mc2vr::ps_census
