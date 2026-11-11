// debug_cull_probe — see cull_probe.hpp.

#include "debug/cull_probe.hpp"

#include <windows.h>

#include <safetyhook.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "game/render_shell.hpp"
#include "hooks/site.hpp"
#include "log.hpp"
#include "vr/view_rewrite.hpp"

namespace mc2vr::cull_probe {

namespace {

constexpr uintptr_t kFrustumTestPoints = 0x00857e70u;
constexpr uintptr_t kViewBlockBase = 0x10;    // view ctx = shell + 0x10 + idx * 0xe80
constexpr uintptr_t kViewStride = 0xe80;
constexpr uint32_t kViews = 4;
constexpr uintptr_t kPlaneNormals = 0xcc;     // 6 x vec3
constexpr uintptr_t kPlaneD = 0xb4;           // 6 floats
constexpr int kBins = 18;                     // pitch -90..+90, 10° each; bin kBins = unknown

bool g_enabled = false;
SafetyHookInline g_hook;
SRWLOCK g_lock = SRWLOCK_INIT;

// Per caller: tests and rejects per pitch bin.
struct Caller {
    uintptr_t ret;
    uint32_t tests[kBins + 1], rejects[kBins + 1];
};
constexpr int kCallers = 8;
Caller g_callers[kCallers];
int g_n_callers = 0;

// Per (caller, frustum, plane): rejects per pitch bin.
struct Key {
    uintptr_t ret;
    int32_t frustum;  // offset in its view block, -1 = not in a view block
    int view;         // view block index, -1 = none
    int plane;        // 0..5, -1 = none found (rejected by a test we don't replicate)
    uint32_t rejects[kBins + 1];
};
constexpr int kKeys = 32;
Key g_keys[kKeys];
int g_n_keys = 0;
uint32_t g_dropped = 0;

int pitch_bin()
{
    float p;
    if (!view_rewrite::main_camera_pitch(&p)) return kBins;
    int b = (int)((p + 90.0f) / 10.0f);
    return b < 0 ? 0 : b >= kBins ? kBins - 1 : b;
}

void classify(const uint8_t *f, int32_t *off, int *view)
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(game::render_shell()) + kViewBlockBase;
    const uintptr_t a = reinterpret_cast<uintptr_t>(f);
    *off = -1;
    *view = -1;
    if (a >= base && a < base + kViews * kViewStride) {
        *view = (int)((a - base) / kViewStride);
        *off = (int32_t)((a - base) % kViewStride);
    }
}

// The plane all points are behind (the same test as the original), -1 = none.
int rejecting_plane(const uint8_t *f, const float *pts, uint32_t n)
{
    const float *normals = reinterpret_cast<const float *>(f + kPlaneNormals);
    const float *d = reinterpret_cast<const float *>(f + kPlaneD);
    for (int p = 0; p < 6; p++) {
        const float *nrm = normals + p * 3;
        bool all_out = true;
        for (uint32_t i = 0; i < n && all_out; i++) {
            const float *q = pts + i * 3;
            if (q[0] * nrm[0] + q[1] * nrm[1] + q[2] * nrm[2] + d[p] >= 0.0f) all_out = false;
        }
        if (all_out) return p;
    }
    return -1;
}

Caller *caller_of(uintptr_t ret)
{
    for (int i = 0; i < g_n_callers; i++) {
        if (g_callers[i].ret == ret) return &g_callers[i];
    }
    if (g_n_callers == kCallers) return nullptr;
    Caller &c = g_callers[g_n_callers++];
    std::memset(&c, 0, sizeof c);
    c.ret = ret;
    return &c;
}

Key *key_of(uintptr_t ret, int32_t frustum, int view, int plane)
{
    for (int i = 0; i < g_n_keys; i++) {
        Key &k = g_keys[i];
        if (k.ret == ret && k.frustum == frustum && k.view == view && k.plane == plane) return &k;
    }
    if (g_n_keys == kKeys) return nullptr;
    Key &k = g_keys[g_n_keys++];
    std::memset(&k, 0, sizeof k);
    k.ret = ret;
    k.frustum = frustum;
    k.view = view;
    k.plane = plane;
    return &k;
}

// The original's convention: stdcall, result in AL.
uint32_t __attribute__((stdcall)) test_points(const uint8_t *f, const float *pts, uint32_t n, uint32_t *mask)
{
    const uint32_t r = g_hook.stdcall<uint32_t>(f, pts, n, mask);
    const uintptr_t ret = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    const int bin = pitch_bin();
    const bool rejected = (r & 0xff) == 0;
    AcquireSRWLockExclusive(&g_lock);
    if (Caller *c = caller_of(ret)) {
        c->tests[bin]++;
        if (rejected) c->rejects[bin]++;
    } else {
        g_dropped++;
    }
    if (rejected) {
        int32_t off;
        int view;
        classify(f, &off, &view);
        const int plane = rejecting_plane(f, pts, n);
        if (Key *k = key_of(ret, off, view, plane)) {
            k->rejects[bin]++;
        } else {
            g_dropped++;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return r;
}

void bins_text(const uint32_t *v, char *out, size_t cap)
{
    size_t len = 0;
    out[0] = '\0';
    for (int b = 0; b <= kBins && len + 24 < cap; b++) {
        if (v[b] == 0) continue;
        if (b == kBins) len += std::snprintf(out + len, cap - len, " ?:%u", v[b]);
        else len += std::snprintf(out + len, cap - len, " %+d:%u", -90 + b * 10, v[b]);
    }
}

} // namespace

bool set_enabled(const char *value)
{
    if (std::strcmp(value, "on") == 0) g_enabled = true;
    else if (std::strcmp(value, "off") == 0) g_enabled = false;
    else return false;
    MC2VR_LOG("cullprobe: debug_cull_probe=%s", value);
    return true;
}

void install()
{
    if (!g_enabled) return;
    if (hooks::create_inline(g_hook, kFrustumTestPoints, reinterpret_cast<void *>(&test_points),
                             "cullprobe Frustum_TestPoints")) {
        MC2VR_LOG("cullprobe: wrapped Frustum_TestPoints @ %p", (void *)kFrustumTestPoints);
    }
}

void report_window()
{
    if (!g_enabled) return;
    static const char *const kPlane[6] = {"left", "bottom", "right", "top", "near", "far"};
    AcquireSRWLockExclusive(&g_lock);
    char a[512], b[512];
    for (int i = 0; i < g_n_callers; i++) {
        const Caller &c = g_callers[i];
        bins_text(c.tests, a, sizeof a);
        bins_text(c.rejects, b, sizeof b);
        MC2VR_LOG("cullprobe: caller %08x tests by pitch:%s", (unsigned)c.ret, a);
        MC2VR_LOG("cullprobe: caller %08x rejects by pitch:%s", (unsigned)c.ret, b);
    }
    for (int i = 0; i < g_n_keys; i++) {
        const Key &k = g_keys[i];
        bins_text(k.rejects, a, sizeof a);
        char fr[32];
        if (k.view >= 0) std::snprintf(fr, sizeof fr, "view%d+0x%03x", k.view, (unsigned)k.frustum);
        else std::snprintf(fr, sizeof fr, "other");
        MC2VR_LOG("cullprobe:   %08x %s plane %s:%s", (unsigned)k.ret, fr, k.plane >= 0 ? kPlane[k.plane] : "?", a);
    }
    if (g_dropped) MC2VR_LOG("cullprobe:   not tracked: %u", g_dropped);
    g_n_callers = g_n_keys = 0;
    g_dropped = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace mc2vr::cull_probe
