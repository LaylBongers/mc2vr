// debug_material_census — see material_census.hpp.

#include "debug/material_census.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <initializer_list>

#include "hooks/shader_reimpl.hpp"
#include "log.hpp"

namespace mc2vr::material_census {

namespace {

bool g_enabled = false;
IDirect3DPixelShader9 *g_ps = nullptr;
const char *g_ps_family = nullptr;  // "Metal"/"Refl" family name of the bound PS, or nullptr

struct Material {
    char family[48];
    float fresnelBase, strength, exponent;  // materialData[2].w, [5].x, [5].z
    float diffuse[3], specular[3];          // [0].rgb, [1].rgb of the first draw
    unsigned long long draws;
};
constexpr int kMax = 512;
Material g_mat[kMax];
int g_n = 0;
unsigned long long g_draws = 0, g_overflow = 0;

// The registry name's family: up to the variant/offset ("PgDiffSpecMetalNormFP_pl (shader3...)"
// -> "PgDiffSpecMetalNormFP").
void family_of(const char *name, char *out, size_t size)
{
    size_t n = 0;
    while (name[n] != '\0' && name[n] != ' ' && n + 1 < size) n++;
    std::memcpy(out, name, n);
    out[n] = '\0';
    for (const char *suffix : {"_pl_sl_li", "_pl_sl", "_pl_li", "_sl_li", "_pl", "_sl"}) {
        const size_t k = std::strlen(suffix);
        if (n > k && std::strcmp(out + n - k, suffix) == 0) {
            out[n - k] = '\0';
            break;
        }
    }
}

float q(float v)
{
    return std::round(v * 1000.0f) / 1000.0f;  // distinct up to 1e-3
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
    wcscpy(slash + 1, L"material_census.csv");
    FILE *f = _wfopen(path, L"w");
    if (f == nullptr) return;
    std::fprintf(f, "family,fresnel_base,strength,exponent,draws,diffuse_r,diffuse_g,diffuse_b,spec_r,spec_g,spec_b\n");
    for (int i = 0; i < g_n; i++) {
        const Material &m = g_mat[i];
        std::fprintf(f, "%s,%.3f,%.3f,%.3f,%llu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n", m.family, m.fresnelBase, m.strength,
                     m.exponent, m.draws, m.diffuse[0], m.diffuse[1], m.diffuse[2], m.specular[0], m.specular[1],
                     m.specular[2]);
    }
    std::fclose(f);
}

} // namespace

bool set_enabled(const char *value)
{
    if (std::strcmp(value, "on") == 0) g_enabled = true;
    else if (std::strcmp(value, "off") == 0) g_enabled = false;
    else return false;
    MC2VR_LOG("matcensus: debug_material_census=%s", value);
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
    const char *name = hooks::shader_reimpl::name_of(ps);
    g_ps_family = (name != nullptr && (std::strstr(name, "Metal") != nullptr || std::strstr(name, "Refl") != nullptr))
                      ? name
                      : nullptr;
}

void on_draw(IDirect3DDevice9 *device)
{
    if (!g_enabled || g_ps_family == nullptr || device == nullptr) return;
    float md[6][4];
    if (FAILED(device->GetPixelShaderConstantF(93, &md[0][0], 6))) return;
    g_draws++;
    char family[48];
    family_of(g_ps_family, family, sizeof family);
    const float fb = q(md[2][3]), st = q(md[5][0]), ex = q(md[5][2]);
    for (int i = 0; i < g_n; i++) {
        Material &m = g_mat[i];
        if (m.fresnelBase == fb && m.strength == st && m.exponent == ex && std::strcmp(m.family, family) == 0) {
            m.draws++;
            return;
        }
    }
    if (g_n >= kMax) {
        g_overflow++;
        return;
    }
    Material &m = g_mat[g_n++];
    std::snprintf(m.family, sizeof m.family, "%s", family);
    m.fresnelBase = fb;
    m.strength = st;
    m.exponent = ex;
    for (int c = 0; c < 3; c++) {
        m.diffuse[c] = md[0][c];
        m.specular[c] = md[1][c];
    }
    m.draws = 1;
}

void report_window()
{
    if (!g_enabled) return;
    write_csv();
    MC2VR_LOG("matcensus: %llu reflective-material draws so far, %d distinct settings%s -> material_census.csv",
              g_draws, g_n, g_overflow ? " (table full)" : "");
}

} // namespace mc2vr::material_census
