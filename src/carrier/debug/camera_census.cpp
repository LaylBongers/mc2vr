// Camera-data census — see camera_census.hpp.

#include "debug/camera_census.hpp"

#include <windows.h>
#include <d3d9.h>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "game/render_cmd.hpp"
#include "log.hpp"
#include "vr/eye_replay.hpp"
#include "vr/shader_constants.hpp"
#include "vr/view_rewrite.hpp"

namespace mc2vr::camera_census {

namespace {

using namespace game;

bool g_enabled = false;
char g_skip[32] = "";  // debug_camera_census_skip
uint64_t g_skipped = 0;  // window

constexpr int kMaxSources = 12;
constexpr int kMaxShaders = 512;

// A register range some upload filled, and who did it.
struct Source {
    const char *what;  // static string
    uint16_t reg, count;
};

struct Shader {
    IDirect3DVertexShader9 *vs;
    const Technique *tech;  // a sample technique using it
    const shader_constants::Table *tab;
    int nsrc;
    Source src[kMaxSources];
    bool has_vcd;  // the technique uploads viewContextData (the rewritten channel)
    bool camera;   // a camera-named constant other than viewContextData
    bool skip;     // has the debug_camera_census_skip constant
    bool reported;
    uint32_t draws;  // total
};

Shader g_shaders[kMaxShaders];
std::atomic<int> g_nshaders{0};
uint64_t g_draws = 0, g_draws_unrewritten = 0;  // window

bool camera_named(const char *n)
{
    if (strcmp(n, "viewContextData") == 0) {
        return false;
    }
    char l[32];
    size_t i = 0;
    for (; n[i] && i < sizeof(l) - 1; i++) l[i] = (char)tolower((unsigned char)n[i]);
    l[i] = '\0';
    if (strstr(l, "viewport")) {
        return false;  // a screen-space parameter, not camera data
    }
    return strstr(l, "proj") || strstr(l, "view") || strstr(l, "mvp") || strstr(l, "texgen") ||
           strstr(l, "camera") || strstr(l, "eye");
}

Shader *shader_of(IDirect3DVertexShader9 *vs, const Technique *tech)
{
    const int n = g_nshaders.load(std::memory_order_relaxed);
    for (int i = 0; i < n; i++) {
        if (g_shaders[i].vs == vs) {
            return &g_shaders[i];
        }
    }
    if (n >= kMaxShaders) {
        return nullptr;
    }
    Shader &s = g_shaders[n];
    s = Shader{};
    s.vs = vs;
    s.tech = tech;
    s.tab = shader_constants::of(vs);
    for (int i = 0; s.tab != nullptr && i < s.tab->n; i++) {
        s.camera = s.camera || camera_named(s.tab->c[i].name);
        s.skip = s.skip || (g_skip[0] != '\0' && strcmp(s.tab->c[i].name, g_skip) == 0);
    }
    g_nshaders.store(n + 1, std::memory_order_release);
    return &s;
}

void add_source(Shader &s, const char *what, uint32_t reg, uint32_t count)
{
    if (count == 0) {
        return;
    }
    for (int i = 0; i < s.nsrc; i++) {
        if (s.src[i].what == what && s.src[i].reg == reg && s.src[i].count == count) {
            return;
        }
    }
    if (s.nsrc < kMaxSources) {
        s.src[s.nsrc++] = {what, (uint16_t)reg, (uint16_t)count};
    }
}

// Who filled registers [reg, reg+count)? Comma-separated, or "none seen".
void sources_of(const Shader &s, uint32_t reg, uint32_t count, char *out, size_t size)
{
    int n = 0;
    out[0] = '\0';
    for (int i = 0; i < s.nsrc && n >= 0 && (size_t)n < size; i++) {
        const Source &x = s.src[i];
        if (x.reg < reg + count && reg < (uint32_t)x.reg + x.count) {
            n += snprintf(out + n, size - n, "%s%s c%u-%u", n ? ", " : "", x.what, x.reg, x.reg + x.count - 1);
        }
    }
    if (n == 0) {
        snprintf(out, size, "none seen");
    }
}

} // namespace

bool set_enabled(const char *value)
{
    if (strcmp(value, "on") == 0) g_enabled = true;
    else if (strcmp(value, "off") == 0) g_enabled = false;
    else return false;
    MC2VR_LOG("camcensus: debug_camera_census=%s", value);
    return true;
}

bool set_skip(const char *value)
{
    snprintf(g_skip, sizeof(g_skip), "%s", value);
    g_enabled = true;
    MC2VR_LOG("camcensus: debug_camera_census_skip=%s — main-pass draws of shaders with it are NOT drawn", g_skip);
    return true;
}

bool on_draw(const PgPrimitive *rec, const Technique *tech)
{
    if (!g_enabled || tech == nullptr || rec->vertexShader == nullptr || !view_rewrite::main_pass_active()) {
        return true;
    }
    Shader *s = shader_of(rec->vertexShader, tech);
    if (s == nullptr) {
        return true;
    }
    if (s->skip) {
        g_skipped++;
        return false;
    }
    if (eye_replay::current_pass() != 1) {
        return true;  // census counts the left-eye pass only
    }
    s->draws++;
    g_draws++;
    if (tech->viewContextData.count) {
        s->has_vcd = true;
    }
    // What SubmitToGPU itself uploads for this technique.
    add_source(*s, "viewContextData", tech->viewContextData.start_reg, tech->viewContextData.count);
    add_source(*s, "screenConsts", tech->screenConsts.start_reg, tech->screenConsts.count);
    add_source(*s, "env", tech->envConsts.start_reg, tech->envConsts.count);
    add_source(*s, "material", tech->materialConsts.start_reg, tech->materialConsts.count);
    add_source(*s, "materialExtra", tech->materialExtra.start_reg, tech->materialExtra.count);
    add_source(*s, "globalLight", tech->globalLight.start_reg, tech->globalLight.count);
    add_source(*s, "atmosphere", tech->atmosphere.start_reg, tech->atmosphere.count);
    add_source(*s, "atmosphereVec", tech->atmosphereVec.start_reg, tech->atmosphereVec.count);
    if (rec->vsConstData != nullptr) {
        add_source(*s, "record", tech->recordConsts.start_reg, rec->vsConstCount * 3);
    }
    // The record's command stream: op 2 = VS constants (op 8 screen constants are internal).
    if (const uint32_t *c = rec->cmdStream) {
        for (int guard = 0; guard < 256 && c[0] != 0 && c[0] < kRenderCmdOpCount; guard++) {
            if (c[0] == (uint32_t)RenderCmdOp::SetVSConstants) {
                const auto &cmd = *reinterpret_cast<const rcmd::SetShaderConstants *>(c);
                add_source(*s, "stream", cmd.start_reg, cmd.vec4_count);
            }
            c += kRenderCmdSize[c[0]];
        }
    }
    if (s->camera) {
        g_draws_unrewritten++;
    }
    return true;
}

void report_window()
{
    if (!g_enabled) {
        return;
    }
    const int n = g_nshaders.load(std::memory_order_acquire);
    int camera = 0, camera_no_vcd = 0;
    for (int i = 0; i < n; i++) {
        Shader &s = g_shaders[i];
        if (!s.camera) {
            continue;
        }
        camera++;
        camera_no_vcd += s.has_vcd ? 0 : 1;
        if (s.reported || s.draws == 0) {
            continue;
        }
        s.reported = true;
        char line[900];
        int k = snprintf(line, sizeof(line), "camcensus: vs=%p tech=%p draws=%u viewContextData=%s |", (void *)s.vs,
                         (const void *)s.tech, s.draws, s.has_vcd ? "yes" : "NO");
        for (int c = 0; s.tab != nullptr && c < s.tab->n && k > 0 && (size_t)k < sizeof(line) - 80; c++) {
            const shader_constants::Named &x = s.tab->c[c];
            char src[200];
            sources_of(s, x.reg, x.count, src, sizeof(src));
            const bool cam = camera_named(x.name) || strcmp(x.name, "viewContextData") == 0;
            k += snprintf(line + k, sizeof(line) - k, " %s%s=c%u-%u <- %s;", cam ? "*" : "", x.name, x.reg,
                          x.reg + x.count - 1, src);
        }
        MC2VR_LOG("%s", line);
    }
    MC2VR_LOG("camcensus: window main-pass draws=%llu with non-viewContextData camera constants=%llu | shaders "
              "seen=%d, with such constants=%d (of them without viewContextData=%d)",
              (unsigned long long)g_draws, (unsigned long long)g_draws_unrewritten, n, camera, camera_no_vcd);
    if (g_skip[0] != '\0') {
        MC2VR_LOG("camcensus: skipped %llu draws (both eyes) with %s", (unsigned long long)g_skipped, g_skip);
    }
    g_draws = g_draws_unrewritten = g_skipped = 0;
}

} // namespace mc2vr::camera_census
