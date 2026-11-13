// ViewContext_FitShadowCascades — see fit_shadow_cascades.hpp. The stock path
// follows the original (0x00859ad0-0x0085a3ee) block by block, in its float
// operation order; deviations: the Matrix_Copy3x4 copies are dropped (the
// original copies the look-at result in, inverts it and copies it back:
// the net effect is the inverse) and the output copies are direct writes.

#include "reimpl/render/fit_shadow_cascades.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "game/render_shell.hpp"
#include "game/view_context.hpp"
#include "log.hpp"

namespace mc2vr::reimpl {

namespace {

using game::ViewContext;

// The original's constants (.rdata).
constexpr float kFltMax = 3.4028235e38f;   // [0x00bea6e4] min-search start
constexpr float kFltLow = -3.4028235e38f;  // [0x00bea788] max-search start
constexpr float kCasterPullback = 300.0f;  // [0x00bad260] light-space z near = min z - this
constexpr float kOne = 1.0f;               // [0x00b9b664]
constexpr float kTwo = 2.0f;               // [0x00b92874]
constexpr float kNegZero = -0.0f;          // [0x00bea764]: `-0.0 - x` negates
constexpr float kNegOne = -1.0f;           // [0x00dfcb5c]
constexpr float kHalf = 0.5f;              // [0x00bbb98c]
constexpr float kQuarter = 0.25f;          // [0x00bbc7dc]
constexpr float kEighth = 0.125f;          // [0x00ba904c]
constexpr float kNegEighth = -0.125f;      // [0x00beac80]

// The stabilized fit's parameters (docs/shadows.md).
constexpr float kCascadeTexels = 1024.0f;  // each cascade's atlas quarter is 1024²
constexpr float kOriginQuantum = 256.0f;   // world units: light-space origin grid = N_i whole texels
constexpr float kRescaleSlack = 0.05f;     // keep a texel size while it wastes at most this much
constexpr float kPi = 3.14159265358979f;

enum class Mode { Stable, Stock };
Mode g_mode = Mode::Stable;

// Window counters (render thread writes, the report poller reads; racy like
// the other window reports).
uint32_t g_fits = 0;
uint32_t g_sun_updates = 0;
float g_sun_drift_max = 0.0f;  // degrees, largest per-fit sun change seen
float g_sun_path = 0.0f;       // degrees, sum of the per-fit changes (path ~ net: a blend; path >> net: jitter)
float g_sun_window_start[3];   // the window's first sun, for the net change
bool g_sun_window_started = false;
uint32_t g_rescales = 0;
// Self-check against the original (first kSelfChecks fits, whatever the mode).
constexpr uint32_t kSelfChecks = 240;
uint32_t g_checks = 0;
uint32_t g_check_bad = 0;      // fits with any output bit different
uint32_t g_check_bad_off = 0;  // first differing ctx offset of the last bad fit
float g_check_bad_ours = 0.0f, g_check_bad_orig = 0.0f;
bool g_check_reported = false;

float take_min(float p, float m) { return (p > m) ? m : p; }  // comiss p,m; ja keep
float take_max(float p, float m) { return (m > p) ? m : p; }  // comiss m,p; ja keep

void transform(float out[3], const float *v, const float *m)
{
    game::D3DXVec3TransformCoord(out, v, m);
}

const float *slice_corners(const ViewContext *ctx, uint32_t i)
{
    return reinterpret_cast<const float *>(ctx->cascades + i * game::kCascadeFrustumStride +
                                           game::kCascadeFrustumCorners);
}

// Per-view state: the stable fit's texel counts and depth range (kept while
// they still fit, so float noise in the radii never rescales a cascade), and
// the view's window stats (both modes).
struct ViewState {
    const ViewContext *ctx;
    uint32_t n[4];   // texel size = kOriginQuantum / n
    float depthR;    // whole-range radius in use
    // window stats
    uint32_t fits, grow, shrink;
    float texMin[4], texMax[4];      // world texel size per cascade
    float nearMin, nearMax, thMin, thMax;
    float sunDevMax;                 // degrees between this view's sun and the light in use
};
ViewState g_views[8];
uint32_t g_view_next = 0;

void reset_stats(ViewState &v)
{
    v.fits = v.grow = v.shrink = 0;
    for (uint32_t i = 0; i < 4; i++) {
        v.texMin[i] = kFltMax;
        v.texMax[i] = 0.0f;
    }
    v.nearMin = v.thMin = kFltMax;
    v.nearMax = v.thMax = 0.0f;
    v.sunDevMax = 0.0f;
}

ViewState &view_state(const ViewContext *ctx)
{
    for (ViewState &v : g_views) {
        if (v.ctx == ctx) return v;
    }
    ViewState &v = g_views[g_view_next++ % 8];
    v = ViewState{};
    v.ctx = ctx;
    reset_stats(v);
    return v;
}

void note_texel(ViewState *v, uint32_t i, float world_texel)
{
    if (v == nullptr) return;
    if (world_texel < v->texMin[i]) v->texMin[i] = world_texel;
    if (world_texel > v->texMax[i]) v->texMax[i] = world_texel;
}

// ---- the original ---------------------------------------------------------------------

// stats = the view's window stats (null for the self-check's copy).
void fit_stock(ViewContext *ctx, const uint8_t *shell, ViewState *stats)
{
    // ---- light view 0x859ae5-0x859b7b ----
    float sun[3];
    std::memcpy(sun, shell + game::kShellSunDirNeg, sizeof sun);
    const float eye[3] = {ctx->cameraWorld[12], ctx->cameraWorld[13], ctx->cameraWorld[14]};
    float target[3];
    target[0] = eye[0] + sun[0];
    target[1] = sun[1] + eye[1];
    target[2] = sun[2] + eye[2];
    float world[16];
    game::Matrix_LookAtToWorld(target, world, eye);
    float lv[16];
    game::Matrix_Inverse4x4(world, lv);

    // ---- whole shadow range: light-space AABB 0x859b80-0x859c74 ----
    float mnx = kFltMax, mny = kFltMax, mnz = kFltMax;
    float mxx = kFltLow, mxy = kFltLow, mxz = kFltLow;
    for (uint32_t k = 0; k < 8; k++) {
        float p[3];
        transform(p, ctx->frustumShadow.corners[k], lv);
        mnx = take_min(p[0], mnx);
        mny = take_min(p[1], mny);
        mnz = take_min(p[2], mnz);
        mxx = take_max(p[0], mxx);
        mxy = take_max(p[1], mxy);
        mxz = take_max(p[2], mxz);
    }

    // ---- base ortho 0x859c7a-0x859d99 ----
    const float zn = mnz - kCasterPullback;
    float o[16] = {};
    const float dx = mxx - mnx;
    o[0] = kTwo / dx;
    o[12] = kNegZero - (mxx + mnx) / dx;
    const float dy = mxy - mny;
    o[5] = kTwo / dy;
    o[13] = kNegZero - (mxy + mny) / dy;
    const float dz = mxz - zn;
    o[10] = kOne / dz;
    o[14] = (kNegOne / dz) * zn;
    o[15] = kOne;
    float base[16];
    game::D3DXMatrixMultiply(base, lv, o);

    // ---- per cascade 0x859dae-0x85a2d0 ----
    for (uint32_t i = 0; i < 4; i++) {
        const float *c = slice_corners(ctx, i);
        float cnx = kFltMax, cny = kFltMax, cxx = kFltLow, cxy = kFltLow;
        for (uint32_t k = 0; k < 8; k++) {
            float p[3];
            transform(p, c + 3 * k, base);
            cnx = take_min(p[0], cnx);
            cny = take_min(p[1], cny);
            cxx = take_max(p[0], cxx);
            cxy = take_max(p[1], cxy);
        }
        const float fi = (float)i * kQuarter;
        const float ddy = cxy - cny;
        const float ddx = cxx - cnx;
        const float oyq = (cxy + cny) / ddy;
        const float oxq = (cxx + cnx) / ddx;
        const float sx = kTwo / ddx;
        const float ox = kNegZero - oxq;
        const float oy = kNegZero - oyq;
        const float sy = kTwo / ddy;
        note_texel(stats, i, ddx / o[0] / kCascadeTexels);  // light-space units (not in the original)
        float *const sb = ctx->cascadeScaleBias[i];
        sb[0] = sx * kHalf;
        sb[1] = sy * kNegEighth;
        sb[2] = (ox + kOne) * kHalf;
        sb[3] = (fi - oy * kEighth) + kEighth;
        float cm[16] = {};
        cm[0] = sx;
        cm[5] = sy;
        cm[10] = kOne;
        cm[12] = ox;
        cm[13] = oy;
        cm[15] = kOne;
        float am[16] = {};
        am[0] = sb[0];
        am[5] = sb[1];
        am[10] = kOne;
        am[12] = sb[2];
        am[13] = sb[3];
        am[15] = kOne;
        game::D3DXMatrixMultiply(ctx->cascadeViewProj[i], base, cm);
        game::D3DXMatrixMultiply(ctx->cascadeAtlas[i], base, am);
    }
    std::memcpy(ctx->shadowBase, base, sizeof base);
}

// ---- the stabilized fit ---------------------------------------------------------------

struct Sphere {
    float c[3];
    float r;
};

float dist(const float *a, const float *b)
{
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

// Bounding sphere of a frustum slice (8 corners: near 0..3, far 4..7): the
// centre on the axis between the near and far quad centres where the near and
// far corners are equidistant (clamped to the slice), the radius the farthest
// corner. Built only from distances, so it doesn't change with the rotation.
Sphere slice_sphere(const float *corners)
{
    float nc[3] = {}, fc[3] = {};
    for (uint32_t k = 0; k < 4; k++) {
        for (uint32_t j = 0; j < 3; j++) {
            nc[j] += corners[3 * k + j] * 0.25f;
            fc[j] += corners[3 * (k + 4) + j] * 0.25f;
        }
    }
    float a = 0.0f, b = 0.0f;
    for (uint32_t k = 0; k < 4; k++) {
        a = std::fmax(a, dist(corners + 3 * k, nc));
        b = std::fmax(b, dist(corners + 3 * (k + 4), fc));
    }
    const float len = dist(fc, nc);
    float t = 0.0f;
    if (len > 0.0f) {
        t = (len * len + b * b - a * a) / (2.0f * len);
        t = std::fmin(std::fmax(t, 0.0f), len);
    }
    Sphere s;
    for (uint32_t j = 0; j < 3; j++) {
        s.c[j] = (len > 0.0f) ? nc[j] + (fc[j] - nc[j]) * (t / len) : nc[j];
    }
    s.r = 0.0f;
    for (uint32_t k = 0; k < 8; k++) s.r = std::fmax(s.r, dist(corners + 3 * k, s.c));
    return s;
}

// The texel count N for a cascade whose sphere needs texels of at least
// `texel` units: kept while N's texel still covers it and wastes at most the
// slack; otherwise the largest N with half the slack as headroom.
uint32_t pick_texel_count(ViewState &st, uint32_t prev, float texel)
{
    const float need = kOriginQuantum / texel;  // the largest N that still covers
    if (prev != 0 && (float)prev <= need && (float)prev >= need * (1.0f - kRescaleSlack)) return prev;
    const float n = std::floor(need * (1.0f - 0.5f * kRescaleSlack));
    if (prev != 0) {
        g_rescales++;
        if ((float)prev > need) st.grow++;  // the sphere outgrew the texels
        else st.shrink++;                   // the texels waste more than the slack
    }
    return n < 1.0f ? 1u : (uint32_t)n;
}

float g_sun_last[3];  // the previous fit's sun, for the stats only

void fit_stable(ViewContext *ctx, const uint8_t *shell, ViewState &st)
{
    // ---- light view: rotation only, along the game's sun direction ----
    // Tracked exactly: a still sun gives bit-identical matrices, and the
    // region blends (the environment regions move the sun over several
    // seconds) then move the shadows smoothly instead of in steps.
    float sun[3];
    std::memcpy(sun, shell + game::kShellSunDirNeg, sizeof sun);
    if (!g_sun_window_started) {
        std::memcpy(g_sun_window_start, sun, sizeof sun);
        g_sun_window_started = true;
    }
    if (std::memcmp(sun, g_sun_last, sizeof sun) != 0) {
        const float la = std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
        const float lb = std::sqrt(g_sun_last[0] * g_sun_last[0] + g_sun_last[1] * g_sun_last[1] +
                                   g_sun_last[2] * g_sun_last[2]);
        if (la > 0.0f && lb > 0.0f) {
            const float d = (sun[0] * g_sun_last[0] + sun[1] * g_sun_last[1] + sun[2] * g_sun_last[2]) / (la * lb);
            const float deg = std::acos(std::fmin(d, 1.0f)) * (180.0f / kPi);
            if (deg > g_sun_drift_max) g_sun_drift_max = deg;
            if (deg > st.sunDevMax) st.sunDevMax = deg;
            g_sun_path += deg;
        }
        std::memcpy(g_sun_last, sun, sizeof sun);
        g_sun_updates++;
    }
    const float origin[3] = {0.0f, 0.0f, 0.0f};
    float world[16];
    game::Matrix_LookAtToWorld(sun, world, origin);
    float lv[16];
    game::Matrix_Inverse4x4(world, lv);

    // Light-space origin on a kOriginQuantum grid near the camera: keeps the
    // coordinates (and depths) small without moving the texel grid.
    float cam_ls[3];
    transform(cam_ls, ctx->cameraWorld + 12, lv);
    lv[12] -= kOriginQuantum * std::nearbyint(cam_ls[0] / kOriginQuantum);
    lv[13] -= kOriginQuantum * std::nearbyint(cam_ls[1] / kOriginQuantum);
    lv[14] -= kOriginQuantum * std::nearbyint(cam_ls[2] / kOriginQuantum);

    // ---- base ortho: x/y the tight AABB of the whole range (as the original;
    // only shadowBase/cascadeScaleBias refer to it), z from its bounding sphere ----
    float mnx = kFltMax, mny = kFltMax, mxx = kFltLow, mxy = kFltLow;
    for (uint32_t k = 0; k < 8; k++) {
        float p[3];
        transform(p, ctx->frustumShadow.corners[k], lv);
        mnx = take_min(p[0], mnx);
        mny = take_min(p[1], mny);
        mxx = take_max(p[0], mxx);
        mxy = take_max(p[1], mxy);
    }
    const Sphere whole = slice_sphere(&ctx->frustumShadow.corners[0][0]);
    if (!(st.depthR >= whole.r && st.depthR <= whole.r * (1.0f + kRescaleSlack))) {
        st.depthR = whole.r * (1.0f + 0.5f * kRescaleSlack);
    }
    float whole_ls[3];
    transform(whole_ls, whole.c, lv);
    const float zn = whole_ls[2] - st.depthR - kCasterPullback;
    const float dz = 2.0f * st.depthR + kCasterPullback;
    float o[16] = {};
    o[0] = kTwo / (mxx - mnx);
    o[12] = kNegZero - (mxx + mnx) / (mxx - mnx);
    o[5] = kTwo / (mxy - mny);
    o[13] = kNegZero - (mxy + mny) / (mxy - mny);
    o[10] = kOne / dz;
    o[14] = kNegZero - zn / dz;
    o[15] = kOne;
    float base[16];
    game::D3DXMatrixMultiply(base, lv, o);

    // ---- per cascade: sphere fit, whole texels ----
    for (uint32_t i = 0; i < 4; i++) {
        const Sphere s = slice_sphere(slice_corners(ctx, i));
        st.n[i] = pick_texel_count(st, st.n[i], 2.0f * s.r / kCascadeTexels);
        const float texel = kOriginQuantum / (float)st.n[i];
        const float half = texel * (0.5f * kCascadeTexels);  // the ortho half-extent
        float c[3];
        transform(c, s.c, lv);
        const float cx = texel * std::nearbyint(c[0] / texel);
        const float cy = texel * std::nearbyint(c[1] / texel);
        note_texel(&st, i, texel);

        float cm[16] = {};  // light space -> cascade clip
        cm[0] = kOne / half;
        cm[5] = kOne / half;
        cm[10] = o[10];
        cm[12] = kNegZero - cx / half;
        cm[13] = kNegZero - cy / half;
        cm[14] = o[14];
        cm[15] = kOne;
        game::D3DXMatrixMultiply(ctx->cascadeViewProj[i], lv, cm);

        const float fi = (float)i * kQuarter;
        float am[16] = {};  // cascade clip -> atlas quarter i
        am[0] = kHalf;
        am[5] = kNegEighth;
        am[10] = kOne;
        am[12] = kHalf;
        am[13] = fi + kEighth;
        am[15] = kOne;
        game::D3DXMatrixMultiply(ctx->cascadeAtlas[i], ctx->cascadeViewProj[i], am);

        // The same mapping expressed from base NDC, as the original stores it.
        const float sx = kOne / (o[0] * half);
        const float sy = kOne / (o[5] * half);
        const float ox = kNegZero - (o[12] / o[0] + cx) / half;
        const float oy = kNegZero - (o[13] / o[5] + cy) / half;
        float *const sb = ctx->cascadeScaleBias[i];
        sb[0] = sx * kHalf;
        sb[1] = sy * kNegEighth;
        sb[2] = (ox + kOne) * kHalf;
        sb[3] = (fi - oy * kEighth) + kEighth;
    }
    std::memcpy(ctx->shadowBase, base, sizeof base);
}

// Runs the original and the stock path on copies of the ctx and compares
// every output bit.
void self_check(const ViewContext *ctx, const uint8_t *shell)
{
    static ViewContext orig, ours;  // ~3.7 KB each; render thread only
    std::memcpy(&orig, ctx, sizeof orig);
    std::memcpy(&ours, ctx, sizeof ours);
    game::ViewContext_FitShadowCascades(&orig);
    fit_stock(&ours, shell, nullptr);
    const auto *a = reinterpret_cast<const uint8_t *>(&ours);
    const auto *b = reinterpret_cast<const uint8_t *>(&orig);
    constexpr uint32_t lo = offsetof(ViewContext, cascadeViewProj), hi = offsetof(ViewContext, re70);
    for (uint32_t off = lo; off < hi; off += 4) {
        if (std::memcmp(a + off, b + off, 4) != 0) {
            g_check_bad++;
            g_check_bad_off = off;
            std::memcpy(&g_check_bad_ours, a + off, 4);
            std::memcpy(&g_check_bad_orig, b + off, 4);
            break;
        }
    }
    g_checks++;
}

} // namespace

bool set_shadow_fit(const char *value)
{
    if (std::strcmp(value, "stable") == 0) g_mode = Mode::Stable;
    else if (std::strcmp(value, "stock") == 0) g_mode = Mode::Stock;
    else return false;
    MC2VR_LOG("shadowfit: shadow_fit=%s", value);
    return true;
}

void ViewContext_FitShadowCascades(game::ViewContext *ctx)
{
    const uint8_t *const shell = game::g_RenderShellPtrB::get();
    if (g_checks < kSelfChecks) self_check(ctx, shell);
    ViewState &st = view_state(ctx);
    st.fits++;
    st.nearMin = std::fmin(st.nearMin, ctx->frustum.nearZ);
    st.nearMax = std::fmax(st.nearMax, ctx->frustum.nearZ);
    st.thMin = std::fmin(st.thMin, std::fabs(ctx->tanH));
    st.thMax = std::fmax(st.thMax, std::fabs(ctx->tanH));
    if (g_mode == Mode::Stock) {
        fit_stock(ctx, shell, &st);
    } else {
        fit_stable(ctx, shell, st);
    }
    g_fits++;
}

void shadow_fit_report_window()
{
    if (g_checks >= kSelfChecks && !g_check_reported) {
        // Oracle: bad=0 — the stock path reproduces the original bit for bit.
        MC2VR_LOG("shadowfit: selfcheck vs original: fits=%u bad=%u (last bad: ctx+0x%03x ours=%.9g orig=%.9g)",
                  g_checks, g_check_bad, g_check_bad_off, (double)g_check_bad_ours, (double)g_check_bad_orig);
        g_check_reported = true;
    }
    if (g_fits == 0) return;
    // Oracles (stable): per view, grow/shrink = 0 and each cascade's texel
    // min = max while its near plane and tanH hold (ADS zoom and the camera's
    // near plane change the slices); sunUpdates counts the fits whose sun
    // changed (region blends; sunDriftMax = the largest step between fits,
    // sunDev per view: whether views disagree on it). Stock: the texel ranges show how much the original's
    // fit breathes.
    float sun_net = 0.0f;
    {
        const float *a = g_sun_window_start, *b = g_sun_last;
        const float la = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        const float lb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
        if (g_sun_window_started && la > 0.0f && lb > 0.0f) {
            const float d = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (la * lb);
            sun_net = std::acos(std::fmin(d, 1.0f)) * (180.0f / kPi);
        }
    }
    MC2VR_LOG("shadowfit: window: mode=%s fits=%u sunUpdates=%u sunDriftMax=%.3f° sunPath=%.2f° sunNet=%.2f° "
              "rescales=%u",
              g_mode == Mode::Stable ? "stable" : "stock", g_fits, g_sun_updates, (double)g_sun_drift_max,
              (double)g_sun_path, (double)sun_net, g_rescales);
    for (ViewState &v : g_views) {
        if (v.ctx == nullptr || v.fits == 0) continue;
        MC2VR_LOG("shadowfit:   view ctx=%p fits=%u grow=%u shrink=%u sunDev=%.3f° near %.3f-%.3f tanH %.3f-%.3f | "
                  "texel cm c0 %.2f-%.2f c1 %.2f-%.2f c2 %.2f-%.2f c3 %.2f-%.2f",
                  (const void *)v.ctx, v.fits, v.grow, v.shrink, (double)v.sunDevMax, (double)v.nearMin,
                  (double)v.nearMax, (double)v.thMin, (double)v.thMax, v.texMin[0] * 100.0, v.texMax[0] * 100.0,
                  v.texMin[1] * 100.0, v.texMax[1] * 100.0, v.texMin[2] * 100.0, v.texMax[2] * 100.0,
                  v.texMin[3] * 100.0, v.texMax[3] * 100.0);
        reset_stats(v);
    }
    g_fits = 0;
    g_sun_updates = 0;
    g_sun_drift_max = 0.0f;
    g_sun_path = 0.0f;
    g_sun_window_started = false;
    g_rescales = 0;
}

} // namespace mc2vr::reimpl
