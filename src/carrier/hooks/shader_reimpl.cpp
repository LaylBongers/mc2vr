// Reimplemented pixel shaders: the replacement list and the substitution —
// see shader_reimpl.hpp. One line per reimplemented shader.

#include "hooks/shader_reimpl.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

#include "log.hpp"
#include "reimpl/render/shadow_surfaces.hpp"
#include "shaders/PgBillboardTreeFP.h"
#include "shaders/PgDecal2FP.h"
#include "shaders/PgDecal2FP_pl.h"
#include "shaders/PgDecal2FP_pl_li.h"
#include "shaders/PgDecal2FP_pl_sl.h"
#include "shaders/PgDecal2FP_pl_sl_li.h"
#include "shaders/PgDecal2FP_sl.h"
#include "shaders/PgDecal2FP_sl_li.h"
#include "shaders/PgFastFP.h"
#include "shaders/PgFastFP_pl.h"
#include "shaders/PgFastFP_pl_li.h"
#include "shaders/PgFastFP_pl_sl.h"
#include "shaders/PgFastFP_pl_sl_li.h"
#include "shaders/PgFastFP_sl.h"
#include "shaders/PgFastFP_sl_li.h"
#include "shaders/PgScrubLightFP.h"
#include "shaders/PgUnderwaterFP.h"
#include "shaders/PgWaterFP.h"
#include "shaders/PgWaterFPDWE.h"
#include "shaders/PgWaterFP_LI.h"
#include "shaders/PgWaterFPbl.h"
#include "shaders/PgScrubLightFP_pl.h"
#include "shaders/PgScrubLightFP_pl_li.h"
#include "shaders/PgScrubLightFP_pl_sl.h"
#include "shaders/PgScrubLightFP_pl_sl_li.h"
#include "shaders/PgScrubLightFP_sl.h"
#include "shaders/PgScrubLightFP_sl_li.h"

namespace mc2vr::hooks::shader_reimpl {

namespace {

struct Entry {
    uint32_t original;     // FNV-1a of the original's bytecode
    const char *name;
    const uint32_t *code;  // ours
    uint32_t created;      // substitutions so far
    bool failed;           // the device refused ours
    const uint32_t *last_used = nullptr;  // what was created for it last (ours, maybe texel-scaled)
};

// One line per reimplemented shader; docs/shaders.md lists them.
Entry g_entries[] = {
    // PgBillboardTreeFP and PgBillboardTreeFP_fade share one bytecode (shader3.bin@0x2ff0, @0x3790).
    {0xeacf64f8u, "PgBillboardTreeFP (shader3.bin@0x2ff0)", mc2vr::shaders::PgBillboardTreeFP, 0, false},
    {0xdeef11a9u, "PgDecal2FP (shader3.bin@0xa2a0)", mc2vr::shaders::PgDecal2FP, 0, false},
    {0x8360631bu, "PgDecal2FP_pl (shader3.bin@0xa840)", mc2vr::shaders::PgDecal2FP_pl, 0, false},
    {0xe40c452eu, "PgDecal2FP_pl_li (shader3.bin@0xb800)", mc2vr::shaders::PgDecal2FP_pl_li, 0, false},
    {0x6751d4d3u, "PgDecal2FP_pl_sl (shader3.bin@0xc210)", mc2vr::shaders::PgDecal2FP_pl_sl, 0, false},
    {0x4552f636u, "PgDecal2FP_pl_sl_li (shader3.bin@0xddd0)", mc2vr::shaders::PgDecal2FP_pl_sl_li, 0, false},
    {0x15cf77adu, "PgDecal2FP_sl (shader3.bin@0xeed0)", mc2vr::shaders::PgDecal2FP_sl, 0, false},
    {0xe651a6c7u, "PgDecal2FP_sl_li (shader3.bin@0x10070)", mc2vr::shaders::PgDecal2FP_sl_li, 0, false},
    {0x3e76835cu, "PgFastFP (shader3.bin@0x1d6370)", mc2vr::shaders::PgFastFP, 0, false},
    {0x1055364cu, "PgFastFP_pl (shader3.bin@0x1d6930)", mc2vr::shaders::PgFastFP_pl, 0, false},
    {0x19b8861cu, "PgFastFP_pl_li (shader3.bin@0x1d7920)", mc2vr::shaders::PgFastFP_pl_li, 0, false},
    {0x2aa895fbu, "PgFastFP_pl_sl (shader3.bin@0x1d8350)", mc2vr::shaders::PgFastFP_pl_sl, 0, false},
    {0x7d01c4f5u, "PgFastFP_pl_sl_li (shader3.bin@0x1d9f40)", mc2vr::shaders::PgFastFP_pl_sl_li, 0, false},
    {0x8d347471u, "PgFastFP_sl (shader3.bin@0x1db080)", mc2vr::shaders::PgFastFP_sl, 0, false},
    {0x5fa41029u, "PgFastFP_sl_li (shader3.bin@0x1dc250)", mc2vr::shaders::PgFastFP_sl_li, 0, false},
    {0x765f3420u, "PgScrubLightFP (shader3.bin@0x2004c0)", mc2vr::shaders::PgScrubLightFP, 0, false},
    {0x523d88a3u, "PgScrubLightFP_pl (shader3.bin@0x200a80)", mc2vr::shaders::PgScrubLightFP_pl, 0, false},
    {0x4249bd13u, "PgScrubLightFP_pl_li (shader3.bin@0x201a60)", mc2vr::shaders::PgScrubLightFP_pl_li, 0, false},
    {0x788c2fddu, "PgScrubLightFP_pl_sl (shader3.bin@0x202480)", mc2vr::shaders::PgScrubLightFP_pl_sl, 0, false},
    {0x59ca705eu, "PgScrubLightFP_pl_sl_li (shader3.bin@0x204060)", mc2vr::shaders::PgScrubLightFP_pl_sl_li, 0, false},
    {0x9de607f6u, "PgScrubLightFP_sl (shader3.bin@0x205180)", mc2vr::shaders::PgScrubLightFP_sl, 0, false},
    {0x307dcccdu, "PgScrubLightFP_sl_li (shader3.bin@0x206340)", mc2vr::shaders::PgScrubLightFP_sl_li, 0, false},
    {0xedb3eaeeu, "PgUnderwaterFP (shader3.bin@0x24f4d0)", mc2vr::shaders::PgUnderwaterFP, 0, false},
    {0xc2dde179u, "PgWaterFPDWE (shader3.bin@0x254a80)", mc2vr::shaders::PgWaterFPDWE, 0, false},
    {0x06f59bd8u, "PgWaterFP (shader3.bin@0x2571d0)", mc2vr::shaders::PgWaterFP, 0, false},
    {0x7050482eu, "PgWaterFP_LI (shader3.bin@0x25c3d0)", mc2vr::shaders::PgWaterFP_LI, 0, false},
    {0xdcf34dbdu, "PgWaterFPbl (shader3.bin@0x24fe80)", mc2vr::shaders::PgWaterFPbl, 0, false},
};

bool g_enabled = true;
uint32_t g_seen = 0, g_reported_seen = 0;  // CreatePixelShader calls

// The bytecode's length in bytes: tokens up to and including the end token
// (comments skipped by their length), 0 if it doesn't look like SM2/3.
uint32_t bytecode_size(const uint32_t *t)
{
    if ((t[0] & 0xffff0000u) != 0xffff0000u) return 0;
    constexpr uint32_t kMaxWords = 1u << 18;
    uint32_t i = 1;
    while (i < kMaxWords) {
        const uint32_t op = t[i] & 0xffff;
        if (t[i] == 0x0000ffffu) return (i + 1) * 4;
        if (op == 0xfffe) {
            i += 1 + ((t[i] >> 16) & 0x7fff);
        } else {
            i += 1 + ((t[i] >> 24) & 0xf);
        }
    }
    return 0;
}

uint32_t fnv1a(const uint8_t *p, uint32_t n)
{
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

} // namespace

bool set_enabled(const char *value)
{
    if (std::strcmp(value, "on") == 0) g_enabled = true;
    else if (std::strcmp(value, "off") == 0) g_enabled = false;
    else return false;
    MC2VR_LOG("shaders: shader_reimpl=%s", value);
    return true;
}

namespace {

// ---- shadow_resolution: the 9-tap receivers' atlas texel constants ----------------
//
// Every shadow receiver with the 9-tap kernel (the game's 315 and ours) holds
// the atlas texel size in `def` constants: +-0.5/1024, +-1.5/1024, +-0.5/4096,
// +-1.5/4096 (tap offsets) and 1024, 4096 (the bilinear weights' scale), and
// no other shader in shader3.bin uses those values (docs/shadows.md). At a
// cascade edge of k * 1024 the offsets scale by 1/k and the sizes by k (exact:
// powers of two). Matched by exact float bits; a shader qualifies only with
// both half-texel signatures.
std::vector<std::vector<uint32_t> *> g_scaled;  // kept for the device's lifetime (leaked by design)
uint32_t g_scaled_count = 0;

uint32_t f32_bits(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

// `code` with its texel constants scaled, or `code` if none qualify.
const uint32_t *texel_scaled(const uint32_t *code)
{
    const uint32_t cascade = reimpl::shadow_cascade_size();
    if (cascade == 1024 || code == nullptr) return code;
    const uint32_t size = bytecode_size(code);
    if (size == 0) return code;
    const float k = (float)cascade / 1024.0f;
    const float offsets[] = {0.5f / 1024, 1.5f / 1024, 0.5f / 4096, 1.5f / 4096, 1.0f / 1024, 1.0f / 4096};
    const float sizes[] = {1024.0f, 4096.0f};
    auto *out = new std::vector<uint32_t>(code, code + size / 4);
    uint32_t *t = out->data();
    bool sig1024 = false, sig4096 = false;
    int changed = 0;
    for (int pass = 0; pass < 2; pass++) {  // 0: find the signatures; 1: scale
        for (uint32_t i = 1; i < size / 4;) {
            const uint32_t op = t[i] & 0xffff;
            if (t[i] == 0x0000ffffu) break;
            if (op == 0xfffe) {
                i += 1 + ((t[i] >> 16) & 0x7fff);
                continue;
            }
            const uint32_t len = (t[i] >> 24) & 0xf;
            if (op == 81 && len == 5) {  // def c#, x, y, z, w
                for (uint32_t c = 0; c < 4; c++) {
                    uint32_t &v = t[i + 2 + c];
                    const uint32_t mag = v & 0x7fffffffu;
                    if (pass == 0) {
                        sig1024 |= mag == f32_bits(0.5f / 1024);
                        sig4096 |= mag == f32_bits(0.5f / 4096);
                        continue;
                    }
                    for (float o : offsets) {
                        if (mag == f32_bits(o)) {
                            v = (v & 0x80000000u) | f32_bits(o / k);
                            changed++;
                        }
                    }
                    for (float s : sizes) {
                        if (v == f32_bits(s)) {
                            v = f32_bits(s * k);
                            changed++;
                        }
                    }
                }
            }
            i += 1 + len;
        }
        if (pass == 0 && !(sig1024 && sig4096)) break;
    }
    if (changed == 0) {
        delete out;
        return code;
    }
    g_scaled.push_back(out);
    g_scaled_count++;
    if (g_scaled_count == 1) {
        MC2VR_LOG("shaders: shadow_resolution=%u: scaling the 9-tap receivers' atlas texel constants "
                  "(first: %d values; count in the window line)", cascade, changed);
    }
    return out->data();
}

} // namespace

const uint32_t *substitute_pixel_shader(const uint32_t *original)
{
    g_seen++;
    if (original == nullptr) return original;
    const uint32_t size = bytecode_size(original);
    if (size == 0) return original;
    if (g_enabled) {
        const uint32_t hash = fnv1a(reinterpret_cast<const uint8_t *>(original), size);
        for (Entry &e : g_entries) {
            if (e.original != hash || e.failed) continue;
            e.created++;
            if (e.created == 1) {
                MC2VR_LOG("shaders: %08x %s -> reimplementation (%u bytes; later creations counted in the window "
                          "line)",
                          hash, e.name, bytecode_size(e.code));
            }
            e.last_used = texel_scaled(e.code);
            return e.last_used;
        }
    }
    return texel_scaled(original);
}

const uint32_t *on_substitute_failed(const uint32_t *original, const uint32_t *used, long hr)
{
    for (Entry &e : g_entries) {
        if (e.last_used != used || used == nullptr) continue;
        e.failed = true;
        MC2VR_LOG("shaders: ERROR — the device refused the reimplementation of %08x %s (hr=0x%08lx); "
                  "the original is used",
                  e.original, e.name, (unsigned long)hr);
    }
    return texel_scaled(original);
}

void report_window()
{
    // Oracle: each reimplemented shader is substituted once per creation by
    // the game (on load); created=0 for a shader the game never created.
    if (g_seen == g_reported_seen) return;
    g_reported_seen = g_seen;
    char line[1024];  // 27+ entries of " xxxxxxxx=n"
    int n = 0;
    for (const Entry &e : g_entries) {
        n += std::snprintf(line + n, sizeof line - (size_t)n, " %08x=%u%s", e.original, e.created,
                           e.failed ? "(failed)" : "");
        if (n >= (int)sizeof line) break;
    }
    MC2VR_LOG("shaders: window: %s CreatePixelShader calls=%u texelScaled=%u | substituted:%s",
              g_enabled ? "on" : "off", g_seen, g_scaled_count, line);
}

} // namespace mc2vr::hooks::shader_reimpl
