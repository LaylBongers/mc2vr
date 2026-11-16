// debug_light_census=on: the light magnitudes the lit receivers are drawn
// with, to judge the classic shaders' clamp of direct light to 1
// (docs/reverse_engineering/direct_light_clamp.md). On each draw whose bound
// pixel shader is one of our lit reimplementations (PgDiff*, PgFastFP,
// PgScrubLightFP, PgTerrainMesh*, PgDecal2FP; hooks/shader_reimpl.hpp
// name_of), reads back from the device:
//   - the sun colour, globalLightData[1] (c10), and the ambient cube's
//     brightest channel ([3..8], c12..c17);
//   - the active local lights' colours: point lights (c36 + 3k + 1) and spot
//     lights (c60 + 4k + 2), the count decoded from the bools b0..b6 as the
//     variant counts them (reimpl/shaders/lights.hlsli);
//   - PgDiff's materialData[0] (c93, the diffuse scale after the clamp).
// Counts per draw: histograms of the brightest channel, the distinct sun and
// local colours, and how many draws could clip (sun alone > 1; sun + the
// brightest local > 1; sun + all locals > 1, an upper bound). Run totals go to
// light_census.csv next to the carrier, rewritten each window. Render thread;
// inert unless enabled.
#pragma once

#include <windows.h>
#include <d3d9.h>  // types only

namespace mc2vr::light_census {

bool set_enabled(const char *value);
bool enabled();

void on_set_ps(IDirect3DPixelShader9 *ps);
void on_draw(IDirect3DDevice9 *device);

// 10 s window report (render_dump's poller).
void report_window();

} // namespace mc2vr::light_census
