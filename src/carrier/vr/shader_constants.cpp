// See shader_constants.hpp.

#include "vr/shader_constants.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace mc2vr::shader_constants {

namespace {

constexpr uint32_t kSlots = 1024;  // open addressing, power of two

struct Entry {
    const void *shader;  // IDirect3DVertexShader9 / IDirect3DPixelShader9
    Table table;
};
Entry g_entries[kSlots];
uint32_t g_used = 0;

// D3D9 constant table in the bytecode's comment block (D3DXSHADER_CONSTANTTABLE:
// Size, Creator, Version, Constants, ConstantInfo, Flags, Target; infos are
// {Name, RegisterSet:16, RegisterIndex:16, RegisterCount:16, Reserved:16,
// TypeInfo, DefaultValue}; offsets relative to the table).
void parse(Table &t, const uint8_t *code, uint32_t size)
{
    t.n = 0;
    for (uint32_t pos = 4; pos + 8 <= size; pos += 4) {
        uint32_t token;
        std::memcpy(&token, code + pos, 4);
        if ((token & 0xffff) != 0xfffe) {
            continue;  // not a comment token
        }
        const uint32_t len = (token >> 16) * 4;
        if (pos + 4 + len > size || len < 8 || std::memcmp(code + pos + 4, "CTAB", 4) != 0) {
            pos += len;
            continue;
        }
        const uint8_t *tab = code + pos + 8;
        const uint32_t tlen = len - 4;
        uint32_t nconst, info;
        std::memcpy(&nconst, tab + 0x0c, 4);
        std::memcpy(&info, tab + 0x10, 4);
        for (uint32_t k = 0; k < nconst && t.n < (int)(sizeof(t.c) / sizeof(t.c[0])); k++) {
            const uint32_t e = info + k * 20;
            if (e + 20 > tlen) {
                break;
            }
            uint32_t name_off;
            uint16_t set, reg, count;
            std::memcpy(&name_off, tab + e, 4);
            std::memcpy(&set, tab + e + 4, 2);
            std::memcpy(&reg, tab + e + 6, 2);
            std::memcpy(&count, tab + e + 8, 2);
            if (set != 2 /* D3DXRS_FLOAT4 */ || name_off >= tlen) {
                continue;
            }
            Named &c = t.c[t.n++];
            snprintf(c.name, sizeof(c.name), "%.*s", (int)(tlen - name_off), (const char *)tab + name_off);
            c.reg = reg;
            c.count = count;
        }
        return;
    }
}

} // namespace

const Named *Table::find(const char *name) const
{
    for (int i = 0; i < n; i++) {
        if (strcmp(c[i].name, name) == 0) {
            return &c[i];
        }
    }
    return nullptr;
}

namespace {

// Either shader type: both expose their bytecode through GetFunction.
template <class Shader>
const Table *lookup(Shader *shader)
{
    if (shader == nullptr) {
        return nullptr;
    }
    uint32_t h = ((uint32_t)(uintptr_t)shader >> 4) * 2654435761u;
    for (uint32_t probe = 0; probe < kSlots; probe++) {
        Entry &e = g_entries[(h + probe) & (kSlots - 1)];
        if (e.shader == shader) {
            return &e.table;
        }
        if (e.shader == nullptr) {
            if (g_used >= kSlots / 2) {
                return nullptr;  // keep the table sparse
            }
            g_used++;
            e.shader = shader;
            UINT size = 0;
            if (SUCCEEDED(shader->GetFunction(nullptr, &size)) && size > 0 && size < (1u << 20)) {
                std::vector<uint8_t> buf(size);
                if (SUCCEEDED(shader->GetFunction(buf.data(), &size))) {
                    parse(e.table, buf.data(), size);
                }
            }
            return &e.table;
        }
    }
    return nullptr;
}

} // namespace

const Table *of(IDirect3DVertexShader9 *vs)
{
    return lookup(vs);
}

const Table *of(IDirect3DPixelShader9 *ps)
{
    return lookup(ps);
}

} // namespace mc2vr::shader_constants
