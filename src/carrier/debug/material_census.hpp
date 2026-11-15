// debug_material_census=on: the material constants the reflective PgDiff
// materials (the Metal and Refl families) are drawn with (docs/shaders.md
// "Modern shading"). On each draw whose bound pixel shader is one of our
// Metal/Refl reimplementations (hooks/shader_reimpl.hpp name_of), reads its
// materialData (c93..c98) back from the device and counts the distinct
// (family, [2].w Fresnel base, [5].x reflection strength, [5].z specular
// exponent) settings, with the diffuse ([0]) and specular ([1]) colours of the
// first draw seen. Run totals go to material_census.csv next to the carrier,
// rewritten each window. Render thread; inert unless enabled.
#pragma once

#include <windows.h>
#include <d3d9.h>  // types only

namespace mc2vr::material_census {

bool set_enabled(const char *value);
bool enabled();

void on_set_ps(IDirect3DPixelShader9 *ps);
void on_draw(IDirect3DDevice9 *device);

// 10 s window report (render_dump's poller).
void report_window();

} // namespace mc2vr::material_census
