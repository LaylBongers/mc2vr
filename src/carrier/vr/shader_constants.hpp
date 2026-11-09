// The named float4 constants of a vertex shader, from the constant table
// (CTAB) embedded in its bytecode (IDirect3DVertexShader9::GetFunction):
// which register range holds viewContextData, LocalToProj, ... for the shader
// a record draws with. Parsed once per shader, cached by pointer (render thread).
#pragma once

#include <d3d9.h>

#include <cstdint>

namespace mc2vr::shader_constants {

struct Named {
    char name[32];
    uint16_t reg, count;  // float4 registers
};

struct Table {
    int n;
    Named c[24];
    const Named *find(const char *name) const;  // nullptr if absent
};

// The shader's table (empty when it has none); nullptr only for a null shader
// or when the cache is full.
const Table *of(IDirect3DVertexShader9 *vs);

} // namespace mc2vr::shader_constants
