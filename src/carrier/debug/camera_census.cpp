// Camera-data census — see camera_census.hpp.

#include "debug/camera_census.hpp"

#include <windows.h>
#include <d3d9.h>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "game/render_cmd.hpp"
#include "hooks/frame_tick.hpp"
#include "log.hpp"
#include "vr/eye_replay.hpp"
#include "vr/shader_constants.hpp"
#include "vr/view_rewrite.hpp"

namespace mc2vr::camera_census {

namespace {

using namespace game;

bool g_enabled = false;
char g_skip[32] = "";  // debug_camera_census_skip (a constant name, or "ps:<n> hashes")
constexpr int kMaxSkipHashes = 64;
uint32_t g_skip_hashes[kMaxSkipHashes];  // debug_camera_census_skip=ps:<hash>,...[;<hash>,...]
int g_skip_hash_group[kMaxSkipHashes];   // group of each hash (';' separates groups)
int g_skip_hash_n = 0;
int g_skip_groups = 0;
// With several groups the skip cycles: one phase nothing hidden, then
// group 0 hidden, nothing, group 1, ... (phase changes logged when phases are
// 5 s or longer). Phase length: debug_camera_census_skip_phase_ms (default
// 15 s); setting it also makes a single group blink (hidden, shown, ...).
uint64_t g_skip_phase_ms = 15000;
bool g_skip_phase_set = false;
uint64_t g_skip_t0 = 0;
int g_skip_phase = -1;

// The hash's group, or -1.
int skip_group(uint32_t h)
{
    for (int i = 0; i < g_skip_hash_n; i++) {
        if (g_skip_hashes[i] == h) return g_skip_hash_group[i];
    }
    return -1;
}

// The group hidden right now, or -1.
int hidden_group()
{
    if (g_skip_groups == 0) return -1;
    if (g_skip_groups == 1 && !g_skip_phase_set) return 0;
    const uint64_t now = GetTickCount64();
    if (g_skip_t0 == 0) g_skip_t0 = now;
    const int phase = (int)(((now - g_skip_t0) / g_skip_phase_ms) % (uint64_t)(2 * g_skip_groups));
    const int group = (phase & 1) ? phase / 2 : -1;
    if (phase != g_skip_phase && g_skip_phase_ms >= 5000) {
        g_skip_phase = phase;
        if (group < 0) MC2VR_LOG("camcensus: skip cycle: nothing hidden");
        else MC2VR_LOG("camcensus: skip cycle: hiding group %d", group);
    }
    return group;
}
uint64_t g_skipped = 0;  // window
uint64_t g_skipped_by_hash[kMaxSkipHashes] = {};  // window, per debug_camera_census_skip=ps: hash

constexpr int kMaxSources = 16;
constexpr int kMaxShaders = 512;
constexpr int kMaxWriters = 4;   // distinct device writers kept per PS constant
constexpr int kMaxConsts = 24;   // == shader_constants::Table capacity
constexpr uint32_t kPsRegs = 256;

// A register range some upload filled, and who did it.
struct Source {
    const char *what;  // static string
    uint16_t reg, count;
};

// A device-level SetPixelShaderConstantF caller: the call's return address and
// the next code address up the stack (the wrapper's caller, when `ret` is in a
// wrapper; a heuristic stack scan).
struct Writer {
    uint32_t ret, up;
};

struct Shader {
    const void *shader;     // IDirect3DVertexShader9 / IDirect3DPixelShader9
    bool ps;
    uint32_t hash, size;    // FNV-1a over the bytecode (tools/shader_hash.py)
    const Technique *tech;  // a sample technique using it
    const EffectPass *pass; // a sample pass (PS)
    const shader_constants::Table *tab;
    int nsrc;
    Source src[kMaxSources];
    bool has_vcd;  // VS: the technique uploads viewContextData (the rewritten channel)
    bool camera;   // a camera-named constant (VS: other than viewContextData)
    bool skip;     // has the debug_camera_census_skip constant
    int skipGroup; // its debug_camera_census_skip=ps: group, or -1
    bool reported;
    uint32_t draws;  // total
    // PS: per CTAB constant, the device writers seen at draw time and the
    // oldest age (frames since the write) a draw read.
    Writer writers[kMaxConsts][kMaxWriters];
    uint8_t nwriters[kMaxConsts];
    bool never_written[kMaxConsts];
    uint32_t max_age[kMaxConsts];
};

Shader g_vs[kMaxShaders], g_ps[kMaxShaders];
std::atomic<int> g_nvs{0}, g_nps{0};
uint64_t g_draws = 0, g_draws_unrewritten = 0, g_draws_ps_camera = 0;  // window

// Device shadow of the PS float registers: last writer and frame.
struct RegWrite {
    Writer w;
    uint64_t frame;
};
RegWrite g_ps_regs[kPsRegs];
uint64_t g_ps_uploads = 0;  // window

// Game code (.text, and the SecuROM-relocated sections Stext..securom).
bool game_code(uint32_t a)
{
    return (a >= 0x00401000u && a < 0x00b05000u) || (a >= 0x01a48000u && a < 0x03772000u);
}

// This DLL's image, for labeling carrier call sites.
uintptr_t g_self_base = 0, g_self_end = 0;
void init_self_range()
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery((const void *)&init_self_range, &mbi, sizeof(mbi)) == 0) {
        return;
    }
    const auto base = (uintptr_t)mbi.AllocationBase;
    const auto *dos = (const IMAGE_DOS_HEADER *)base;
    const auto *nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    g_self_base = base;
    g_self_end = base + nt->OptionalHeader.SizeOfImage;
}
bool self_code(uint32_t a)
{
    return a >= g_self_base && a < g_self_end;
}

// "0x00123456", or "mc2vr+0x1234" for a carrier address.
void fmt_addr(uint32_t a, char *out, size_t size)
{
    if (a == 0) snprintf(out, size, "-");
    else if (self_code(a)) snprintf(out, size, "mc2vr+0x%x", (unsigned)(a - g_self_base));
    else snprintf(out, size, "0x%08x", (unsigned)a);
}

bool contains_ci(const char *n, const char *what)
{
    char l[32];
    size_t i = 0;
    for (; n[i] && i < sizeof(l) - 1; i++) l[i] = (char)tolower((unsigned char)n[i]);
    l[i] = '\0';
    return strstr(l, what) != nullptr;
}

bool camera_named(const char *n)
{
    if (strcmp(n, "viewContextData") == 0) {
        return false;
    }
    if (contains_ci(n, "viewport")) {
        return false;  // a screen-space parameter, not camera data
    }
    return contains_ci(n, "proj") || contains_ci(n, "view") || contains_ci(n, "mvp") || contains_ci(n, "texgen") ||
           contains_ci(n, "camera") || contains_ci(n, "eye");
}

// PS: the viewContextData record is camera data too (only the VS copy is
// rewritten); "*Mtx" / "*Mat" matrices are listed for review (light matrices
// among them).
bool camera_named_ps(const char *n)
{
    return strcmp(n, "viewContextData") == 0 || camera_named(n) || contains_ci(n, "mtx") ||
           (strlen(n) > 3 && strcmp(n + strlen(n) - 3, "Mat") == 0);
}

uint32_t fnv1a(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

template <class T> void hash_bytecode(T *shader, uint32_t &hash, uint32_t &size)
{
    UINT n = 0;
    if (FAILED(shader->GetFunction(nullptr, &n)) || n == 0) {
        return;
    }
    auto *buf = (uint8_t *)malloc(n);
    if (buf != nullptr && SUCCEEDED(shader->GetFunction(buf, &n))) {
        hash = fnv1a(buf, n);
        size = n;
    }
    free(buf);
}

template <class T> Shader *shader_of(Shader *arr, std::atomic<int> &count, T *shader, const Technique *tech,
                                     const EffectPass *pass, bool ps)
{
    const int n = count.load(std::memory_order_relaxed);
    for (int i = 0; i < n; i++) {
        if (arr[i].shader == shader) {
            return &arr[i];
        }
    }
    if (n >= kMaxShaders) {
        return nullptr;
    }
    Shader &s = arr[n];
    s = Shader{};
    s.shader = shader;
    s.ps = ps;
    s.tech = tech;
    s.pass = pass;
    s.tab = shader_constants::of(shader);
    hash_bytecode(shader, s.hash, s.size);
    s.skipGroup = (ps && s.hash != 0) ? skip_group(s.hash) : -1;
    for (int i = 0; s.tab != nullptr && i < s.tab->n; i++) {
        const char *name = s.tab->c[i].name;
        s.camera = s.camera || (ps ? camera_named_ps(name) : camera_named(name));
        s.skip = s.skip || (g_skip[0] != '\0' && strcmp(name, g_skip) == 0);
    }
    count.store(n + 1, std::memory_order_release);
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

bool source_overlaps(const Shader &s, const char *what, uint32_t reg, uint32_t count)
{
    for (int i = 0; i < s.nsrc; i++) {
        const Source &x = s.src[i];
        if (strcmp(x.what, what) == 0 && x.reg < reg + count && reg < (uint32_t)x.reg + x.count) {
            return true;
        }
    }
    return false;
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

// The record's command stream: op 2 / op 3 = VS / PS constants.
void add_stream_sources(Shader &s, const PgPrimitive *rec, RenderCmdOp op)
{
    const uint32_t *c = rec->cmdStream;
    for (int guard = 0; c != nullptr && guard < 256 && c[0] != 0 && c[0] < kRenderCmdOpCount; guard++) {
        if (c[0] == (uint32_t)op) {
            const auto &cmd = *reinterpret_cast<const rcmd::SetShaderConstants *>(c);
            add_source(s, "stream", cmd.start_reg, cmd.vec4_count);
        }
        c += kRenderCmdSize[c[0]];
    }
}

void note_writer(Shader &s, int ci, uint32_t reg, uint64_t now)
{
    if (reg >= kPsRegs) {
        return;
    }
    const RegWrite &r = g_ps_regs[reg];
    if (r.w.ret == 0) {
        s.never_written[ci] = true;
        return;
    }
    const uint64_t age = now - r.frame;
    if (age > s.max_age[ci]) {
        s.max_age[ci] = (uint32_t)(age > 0xffffffffu ? 0xffffffffu : age);
    }
    for (int i = 0; i < s.nwriters[ci]; i++) {
        if (s.writers[ci][i].ret == r.w.ret && s.writers[ci][i].up == r.w.up) {
            return;
        }
    }
    if (s.nwriters[ci] < kMaxWriters) {
        s.writers[ci][s.nwriters[ci]++] = r.w;
    }
}

void on_draw_ps(const PgPrimitive *rec, const EffectPass *pass)
{
    IDirect3DDevice9 *const dev = lti_device();
    IDirect3DPixelShader9 *ps = nullptr;
    if (dev == nullptr || FAILED(dev->GetPixelShader(&ps)) || ps == nullptr) {
        return;
    }
    Shader *s = shader_of(g_ps, g_nps, ps, nullptr, pass, true);
    ps->Release();  // keyed on the pointer; the shader lives on in the device
    if (s == nullptr) {
        return;
    }
    s->draws++;
    if (pass != nullptr) {
        // What SubmitToGPU itself uploads for this pass.
        add_source(*s, "screenConstsA", pass->screenConstsA.start_reg, pass->screenConstsA.count);
        add_source(*s, "screenConstsB", pass->screenConstsB.start_reg, pass->screenConstsB.count);
        add_source(*s, "globalLight", pass->globalLight.start_reg, pass->globalLight.count);
        add_source(*s, "materialMatrix", pass->materialMatrix.start_reg, pass->materialMatrix.count);
        add_source(*s, "cameraPos", pass->cameraPos.start_reg, pass->cameraPos.count);
        add_source(*s, "materialExtra", pass->materialExtra.start_reg, pass->materialExtra.count);
        add_source(*s, "screenConsts", pass->screenConsts.start_reg, pass->screenConsts.count);
        add_source(*s, "viewContextData", pass->viewContextData.start_reg, pass->viewContextData.count);
        add_source(*s, "blurAndTint", pass->blurAndTint.start_reg, pass->blurAndTint.count);
    }
    add_stream_sources(*s, rec, RenderCmdOp::SetPSConstants);
    if (s->camera) {
        g_draws_ps_camera++;
    }
    // The device writers of each constant's first and last register.
    const uint64_t now = hooks::frame_count();
    for (int i = 0; s->tab != nullptr && i < s->tab->n && i < kMaxConsts; i++) {
        const shader_constants::Named &x = s->tab->c[i];
        note_writer(*s, i, x.reg, now);
        if (x.count > 1) {
            note_writer(*s, i, x.reg + x.count - 1, now);
        }
    }
}

void report_vs(Shader &s)
{
    char line[900];
    int k = snprintf(line, sizeof(line), "camcensus: vs=%p hash=%08x size=%u tech=%p draws=%u viewContextData=%s |",
                     s.shader, s.hash, s.size, (const void *)s.tech, s.draws, s.has_vcd ? "yes" : "NO");
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

// A PS constant is camera data by name, or by what SubmitToGPU fills it from.
bool ps_const_camera(const Shader &s, const shader_constants::Named &x)
{
    return camera_named_ps(x.name) || source_overlaps(s, "viewContextData", x.reg, x.count) ||
           source_overlaps(s, "cameraPos", x.reg, x.count);
}

void report_ps(Shader &s)
{
    char line[900];
    int k = snprintf(line, sizeof(line), "camcensus: ps=%p hash=%08x size=%u pass=%p draws=%u |", s.shader, s.hash,
                     s.size, (const void *)s.pass, s.draws);
    for (int c = 0; s.tab != nullptr && c < s.tab->n && k > 0 && (size_t)k < sizeof(line) - 80; c++) {
        const shader_constants::Named &x = s.tab->c[c];
        char src[200];
        sources_of(s, x.reg, x.count, src, sizeof(src));
        k += snprintf(line + k, sizeof(line) - k, " %s%s=c%u-%u <- %s;", ps_const_camera(s, x) ? "*" : "", x.name,
                      x.reg, x.reg + x.count - 1, src);
    }
    MC2VR_LOG("%s", line);
    // Device writers per camera constant.
    for (int c = 0; s.tab != nullptr && c < s.tab->n && c < kMaxConsts; c++) {
        const shader_constants::Named &x = s.tab->c[c];
        if (!ps_const_camera(s, x)) {
            continue;
        }
        char w[400];
        int n = 0;
        w[0] = '\0';
        for (int i = 0; i < s.nwriters[c] && n >= 0 && (size_t)n < sizeof(w) - 48; i++) {
            char a[24], b[24];
            fmt_addr(s.writers[c][i].ret, a, sizeof(a));
            fmt_addr(s.writers[c][i].up, b, sizeof(b));
            n += snprintf(w + n, sizeof(w) - n, "%s%s (up %s)", n ? ", " : "", a, b);
        }
        MC2VR_LOG("camcensus:   ps=%p %s c%u-%u device writers: %s%s | max age %u frames", s.shader, x.name, x.reg,
                  x.reg + x.count - 1, n ? w : "none", s.never_written[c] ? " [some draws: never written]" : "",
                  s.max_age[c]);
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

bool enabled()
{
    return g_enabled;
}

bool set_skip_phase(const char *value)
{
    char *end = nullptr;
    const unsigned long ms = strtoul(value, &end, 10);
    if (end == value || ms < 100 || ms > 600000) return false;
    g_skip_phase_ms = ms;
    g_skip_phase_set = true;
    MC2VR_LOG("camcensus: debug_camera_census_skip_phase_ms=%lu — skip groups cycle per %lu ms "
              "(a single group blinks: hidden, shown, ...)", ms, ms);
    return true;
}

bool set_skip(const char *value)
{
    if (strncmp(value, "ps:", 3) == 0) {
        // Pixel-shader bytecode hashes (FNV-1a, as ps_census / tools/shader_hash.py).
        g_skip_hash_n = 0;
        g_skip_groups = 1;
        const char *p = value + 3;
        while (*p != '\0' && g_skip_hash_n < kMaxSkipHashes) {
            char *end = nullptr;
            const unsigned long h = strtoul(p, &end, 16);
            if (end == p) break;
            g_skip_hash_group[g_skip_hash_n] = g_skip_groups - 1;
            g_skip_hashes[g_skip_hash_n++] = (uint32_t)h;
            p = end;
            while (*p == ',' || *p == ' ') p++;
            if (*p == ';') {
                g_skip_groups++;
                p++;
                while (*p == ' ') p++;
            }
        }
        snprintf(g_skip, sizeof(g_skip), "ps:%d hashes/%d groups", g_skip_hash_n, g_skip_groups);
        g_enabled = true;
        MC2VR_LOG("camcensus: debug_camera_census_skip=%s — main-pass draws with these pixel shaders are NOT drawn",
                  g_skip);
        return true;
    }
    snprintf(g_skip, sizeof(g_skip), "%s", value);
    g_enabled = true;
    MC2VR_LOG("camcensus: debug_camera_census_skip=%s — main-pass draws of shaders with it are NOT drawn", g_skip);
    return true;
}

void on_ps_constants(uint32_t start, uint32_t count, uintptr_t ret, const uint32_t *stack)
{
    if (!g_enabled) {
        return;
    }
    if (g_self_end == 0) {
        init_self_range();
    }
    Writer w{(uint32_t)ret, 0};
    for (int i = 0; stack != nullptr && i < 32; i++) {
        const uint32_t a = stack[i];
        if (a != w.ret && (game_code(a) || self_code(a))) {
            w.up = a;
            break;
        }
    }
    const uint64_t frame = hooks::frame_count();
    for (uint32_t r = start; r < start + count && r < kPsRegs; r++) {
        g_ps_regs[r] = {w, frame};
    }
    g_ps_uploads++;
}

bool on_draw(const PgPrimitive *rec, const Technique *tech, const EffectPass *pass)
{
    if (!g_enabled || tech == nullptr || rec->vertexShader == nullptr || !view_rewrite::main_pass_active()) {
        return true;
    }
    Shader *s = shader_of(g_vs, g_nvs, rec->vertexShader, tech, nullptr, false);
    if (s == nullptr) {
        return true;
    }
    if (s->skip) {
        g_skipped++;
        return false;
    }
    if (g_skip[0] != '\0') {
        // The PS side of the skip (both eyes): look the bound PS up.
        IDirect3DDevice9 *const dev = lti_device();
        IDirect3DPixelShader9 *ps = nullptr;
        if (dev != nullptr && SUCCEEDED(dev->GetPixelShader(&ps)) && ps != nullptr) {
            Shader *p = shader_of(g_ps, g_nps, ps, nullptr, pass, true);
            ps->Release();
            if (p != nullptr && (p->skip || (p->skipGroup >= 0 && p->skipGroup == hidden_group()))) {
                g_skipped++;
                for (int i = 0; i < g_skip_hash_n; i++) {
                    if (g_skip_hashes[i] == p->hash) g_skipped_by_hash[i]++;
                }
                return false;
            }
        }
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
    add_stream_sources(*s, rec, RenderCmdOp::SetVSConstants);  // op 8 screen constants are internal
    if (s->camera) {
        g_draws_unrewritten++;
    }
    on_draw_ps(rec, pass);
    return true;
}

void report_window()
{
    if (!g_enabled) {
        return;
    }
    const int nvs = g_nvs.load(std::memory_order_acquire);
    int camera = 0, camera_no_vcd = 0;
    for (int i = 0; i < nvs; i++) {
        Shader &s = g_vs[i];
        if (!s.camera) {
            continue;
        }
        camera++;
        camera_no_vcd += s.has_vcd ? 0 : 1;
        if (!s.reported && s.draws != 0) {
            s.reported = true;
            report_vs(s);
        }
    }
    const int nps = g_nps.load(std::memory_order_acquire);
    int ps_camera = 0;
    for (int i = 0; i < nps; i++) {
        Shader &s = g_ps[i];
        bool cam = s.camera;
        for (int c = 0; !cam && s.tab != nullptr && c < s.tab->n; c++) {
            cam = ps_const_camera(s, s.tab->c[c]);
        }
        if (!cam) {
            continue;
        }
        ps_camera++;
        if (!s.reported && s.draws != 0) {
            s.reported = true;
            report_ps(s);
        }
    }
    MC2VR_LOG("camcensus: window main-pass draws=%llu with non-viewContextData VS camera constants=%llu, with "
              "camera-named PS constants=%llu | VS seen=%d, with such constants=%d (of them without "
              "viewContextData=%d) | PS seen=%d, with camera data=%d | PS uploads=%llu",
              (unsigned long long)g_draws, (unsigned long long)g_draws_unrewritten,
              (unsigned long long)g_draws_ps_camera, nvs, camera, camera_no_vcd, nps, ps_camera,
              (unsigned long long)g_ps_uploads);
    if (g_skip[0] != '\0') {
        MC2VR_LOG("camcensus: skipped %llu draws (both eyes) with %s", (unsigned long long)g_skipped, g_skip);
        // Per pixel-shader hash, those that drew this window.
        char line[512];
        int n = 0;
        for (int i = 0; i < g_skip_hash_n && n < (int)sizeof line - 32; i++) {
            if (g_skipped_by_hash[i] == 0) continue;
            n += snprintf(line + n, sizeof line - (size_t)n, " %08x=%llu", g_skip_hashes[i],
                          (unsigned long long)g_skipped_by_hash[i]);
            g_skipped_by_hash[i] = 0;
        }
        if (n > 0) MC2VR_LOG("camcensus:   by shader:%s", line);
    }
    g_draws = g_draws_unrewritten = g_draws_ps_camera = g_skipped = g_ps_uploads = 0;
}

} // namespace mc2vr::camera_census
