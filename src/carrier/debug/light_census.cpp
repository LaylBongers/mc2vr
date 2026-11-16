// debug_light_census — see light_census.hpp.

#include "debug/light_census.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "hooks/shader_reimpl.hpp"
#include "log.hpp"

namespace mc2vr::light_census {

namespace {

// The bound pixel shader, as far as the census cares.
struct Bound {
    bool tracked = false;   // a lit receiver of ours
    bool points = false;    // _pl variant
    bool spots = false;     // _sl variant
    bool li = false;        // _li counting scheme
    bool pgdiff = false;    // has materialData[0] at c93
};

bool g_enabled = false;
IDirect3DPixelShader9 *g_ps = nullptr;
Bound g_bound;

// Histogram of a brightest channel: bins <= 0.25, 0.5, 0.75, 1, 1.25, 1.5, 2, 3, 4, and > 4.
constexpr float kEdges[] = {0.25f, 0.5f, 0.75f, 1.0f, 1.25f, 1.5f, 2.0f, 3.0f, 4.0f};
constexpr int kBins = sizeof kEdges / sizeof kEdges[0] + 1;
struct Hist {
    unsigned long long n[kBins] = {};
    float max = 0;
    void add(float v)
    {
        int b = 0;
        while (b < kBins - 1 && v > kEdges[b]) b++;
        n[b]++;
        max = std::max(max, v);
    }
    void log(const char *what) const
    {
        char line[256];
        int at = 0;
        for (int b = 0; b < kBins && at < (int)sizeof line; b++) {
            at += std::snprintf(line + at, sizeof line - at, " %llu", n[b]);
        }
        MC2VR_LOG("lightcensus: %-9s max %.3f | bins<=.25/.5/.75/1/1.25/1.5/2/3/4/>4:%s", what, max, line);
    }
};
Hist g_sun, g_ambient, g_local, g_matDiffuse;

// Distinct colours (to 1e-2) with their draw counts.
struct Colour {
    char kind;  // 's' sun, 'p' point, 'o' spot
    float rgb[3];
    unsigned long long draws;
};
constexpr int kMaxColours = 1024;
Colour g_colours[kMaxColours];
int g_ncolours = 0;
unsigned long long g_colour_overflow = 0;

unsigned long long g_draws = 0, g_draws_lit = 0;
// Draws whose direct light could exceed 1 in some channel (the classic clamp
// engages there at full N.L and visibility): the sun alone; the sun plus the
// brightest local light; the sun plus all local lights (an upper bound: every
// light at full N.L at once).
unsigned long long g_clip_sun = 0, g_clip_sun_max = 0, g_clip_sun_sum = 0;

float q(float v)
{
    return std::round(v * 100.0f) / 100.0f;
}

float max3(const float *c)
{
    return std::max(c[0], std::max(c[1], c[2]));
}

void count_colour(char kind, const float *rgb)
{
    const float r = q(rgb[0]), g = q(rgb[1]), b = q(rgb[2]);
    for (int i = 0; i < g_ncolours; i++) {
        Colour &c = g_colours[i];
        if (c.kind == kind && c.rgb[0] == r && c.rgb[1] == g && c.rgb[2] == b) {
            c.draws++;
            return;
        }
    }
    if (g_ncolours >= kMaxColours) {
        g_colour_overflow++;
        return;
    }
    g_colours[g_ncolours++] = {kind, {r, g, b}, 1};
}

bool starts_with(const char *s, const char *prefix)
{
    return std::strncmp(s, prefix, std::strlen(prefix)) == 0;
}

// The variant's active light counts from its bools (reimpl/shaders/lights.hlsli).
int point_count(const BOOL *b, bool li)
{
    if (li) return 2 + (b[2] ? 2 : 0) + (b[2] && b[3] ? 2 : 0);
    return b[3] ? 8 : b[2] ? 4 : b[1] ? 2 : b[0] ? 1 : 0;
}
int spot_count(const BOOL *b, bool li)
{
    if (li) return 2 + (b[6] ? 2 : 0);
    return b[6] ? 4 : b[5] ? 2 : b[4] ? 1 : 0;
}

void write_csv()
{
    wchar_t path[MAX_PATH];
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&write_csv, &self) ||
        !GetModuleFileNameW(self, path, MAX_PATH)) {
        return;
    }
    wchar_t *slash = wcsrchr(path, L'\\');
    if (slash == nullptr) return;
    wcscpy(slash + 1, L"light_census.csv");
    FILE *f = _wfopen(path, L"w");
    if (f == nullptr) return;
    std::fprintf(f, "kind,r,g,b,max_channel,draws\n");
    for (int i = 0; i < g_ncolours; i++) {
        const Colour &c = g_colours[i];
        const char *kind = c.kind == 's' ? "sun" : c.kind == 'p' ? "point" : "spot";
        std::fprintf(f, "%s,%.2f,%.2f,%.2f,%.2f,%llu\n", kind, c.rgb[0], c.rgb[1], c.rgb[2], max3(c.rgb), c.draws);
    }
    std::fclose(f);
}

} // namespace

bool set_enabled(const char *value)
{
    if (std::strcmp(value, "on") == 0) g_enabled = true;
    else if (std::strcmp(value, "off") == 0) g_enabled = false;
    else return false;
    MC2VR_LOG("lightcensus: debug_light_census=%s", value);
    return true;
}

bool enabled()
{
    return g_enabled;
}

void on_set_ps(IDirect3DPixelShader9 *ps)
{
    if (!g_enabled || ps == g_ps) return;
    g_ps = ps;
    g_bound = Bound{};
    const char *name = hooks::shader_reimpl::name_of(ps);
    if (name == nullptr) return;
    // The registry name up to its " (shader3.bin@...)" suffix.
    char base[64];
    size_t n = 0;
    while (name[n] != '\0' && name[n] != ' ' && n + 1 < sizeof base) n++;
    std::memcpy(base, name, n);
    base[n] = '\0';
    if (!starts_with(base, "PgDiff") && !starts_with(base, "PgFastFP") && !starts_with(base, "PgScrubLightFP") &&
        !starts_with(base, "PgTerrainMesh") && !starts_with(base, "PgDecal2FP")) {
        return;
    }
    g_bound.tracked = true;
    g_bound.points = std::strstr(base, "_pl") != nullptr;
    g_bound.spots = std::strstr(base, "_sl") != nullptr;
    g_bound.li = std::strstr(base, "_li") != nullptr;
    g_bound.pgdiff = starts_with(base, "PgDiff") && !starts_with(base, "PgDiffRefract");
}

void on_draw(IDirect3DDevice9 *device)
{
    if (!g_enabled || !g_bound.tracked || device == nullptr) return;
    float gl[9][4];  // globalLightData[0..8] = c9..c17
    if (FAILED(device->GetPixelShaderConstantF(9, &gl[0][0], 9))) return;
    g_draws++;

    const float *sun = gl[1];
    g_sun.add(max3(sun));
    count_colour('s', sun);
    float ambient = 0;
    for (int k = 3; k <= 8; k++) ambient = std::max(ambient, max3(gl[k]));
    g_ambient.add(ambient);

    float sum[3] = {sun[0], sun[1], sun[2]}, brightest[3] = {};
    bool lit = false;
    if (g_bound.points || g_bound.spots) {
        BOOL b[7] = {};
        if (SUCCEEDED(device->GetPixelShaderConstantB(0, b, 7))) {
            const auto add = [&](char kind, const float *c) {
                lit = true;
                g_local.add(max3(c));
                count_colour(kind, c);
                for (int ch = 0; ch < 3; ch++) {
                    sum[ch] += std::max(c[ch], 0.0f);
                    brightest[ch] = std::max(brightest[ch], c[ch]);
                }
            };
            if (g_bound.points) {
                const int count = point_count(b, g_bound.li);
                float pl[24][4];
                if (count > 0 && SUCCEEDED(device->GetPixelShaderConstantF(36, &pl[0][0], 3 * count))) {
                    for (int k = 0; k < count; k++) add('p', pl[3 * k + 1]);
                }
            }
            if (g_bound.spots) {
                const int count = spot_count(b, g_bound.li);
                float sl[16][4];
                if (count > 0 && SUCCEEDED(device->GetPixelShaderConstantF(60, &sl[0][0], 4 * count))) {
                    for (int k = 0; k < count; k++) add('o', sl[4 * k + 2]);
                }
            }
        }
    }
    if (lit) g_draws_lit++;
    if (max3(sun) > 1) g_clip_sun++;
    const float sunMax[3] = {sun[0] + brightest[0], sun[1] + brightest[1], sun[2] + brightest[2]};
    if (max3(sunMax) > 1) g_clip_sun_max++;
    if (max3(sum) > 1) g_clip_sun_sum++;

    if (g_bound.pgdiff) {
        float md[4];
        if (SUCCEEDED(device->GetPixelShaderConstantF(93, md, 1))) g_matDiffuse.add(max3(md));
    }
}

void report_window()
{
    if (!g_enabled) return;
    write_csv();
    MC2VR_LOG("lightcensus: %llu lit-receiver draws, %llu with local lights; could clip: sun %llu, sun+brightest "
              "local %llu, sun+all locals %llu; %d distinct colours%s -> light_census.csv",
              g_draws, g_draws_lit, g_clip_sun, g_clip_sun_max, g_clip_sun_sum, g_ncolours,
              g_colour_overflow ? " (table full)" : "");
    g_sun.log("sun");
    g_ambient.log("ambient");
    g_local.log("local");
    g_matDiffuse.log("matDiff");
}

} // namespace mc2vr::light_census
