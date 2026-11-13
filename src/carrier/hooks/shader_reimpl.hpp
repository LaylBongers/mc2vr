// Reimplemented pixel shaders replacing their originals: the shader
// counterpart of hooks/reimpl.hpp. The reimplementations are the HLSL in
// src/carrier/reimpl/shaders/ (compiled at build time by cmake/hlsl.cmake);
// this is the glue that puts them into the game (docs/shaders.md).
//
// The game creates every pixel shader through IDirect3DDevice9::CreatePixelShader
// from the bytecode in its shader containers. hooks/device.cpp hands each
// creation's bytecode to substitute_pixel_shader, which identifies the
// original by its FNV-1a hash (as tools/shader_hash.py and the censuses) and
// returns ours instead when it is reimplemented. The game keeps the object it
// gets back, so its state caches stay consistent. Our shaders keep the
// originals' interfaces (inputs, constant registers, samplers).
//
// Debug tools that hash bound shaders (ps_census, camera_census) see our
// bytecode's hashes for replaced shaders.
//
// conf shader_reimpl = on (default) | off (the originals, for A/B).
#pragma once

#include <cstdint>

namespace mc2vr::hooks::shader_reimpl {

bool set_enabled(const char *value);

// The bytecode to create instead of `original` (a CreatePixelShader
// argument): ours when the original is reimplemented, else `original`.
const uint32_t *substitute_pixel_shader(const uint32_t *original);

// CreatePixelShader failed with our bytecode for `original` (the hook then
// creates the original): logged, and that entry is disabled.
void on_substitute_failed(const uint32_t *ours, long hr);

// 10 s window report; logs only when something changed.
void report_window();

} // namespace mc2vr::hooks::shader_reimpl
