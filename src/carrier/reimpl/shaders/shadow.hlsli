// The sun's cascaded shadow lookup, as the game's high-quality receivers do it
// (shader3.bin@0x11570, the diffuseMap + materialData shader; docs/shadows.md),
// plus a blend band at the cascade boundaries and a footprint-scaled filter
// (sun_shadow; neither is in the game).
// Shared by the reimplemented receivers in this directory.
// globalLightData's layout: global_light.hlsli.

#ifndef MC2VR_SHADOW_HLSLI
#define MC2VR_SHADOW_HLSLI

#include "global_light.hlsli"

sampler2D globalDirLightShadowMap : register(s15);  // depth atlas, POINT

// The atlas texel at the stock 1024x4096 (four 1024² cascades stacked
// vertically). At another shadow_resolution these constants are rescaled in
// the bytecode at creation (hooks/shader_reimpl.cpp).
static const float2 kShadowTexel = float2(1.0 / 1024.0, 1.0 / 4096.0);
static const float2 kShadowSize = float2(1024.0, 4096.0);

// The cascade the view depth falls in: as many as split ends lie before it
// (4 = past the last split).
float shadow_cascade(float viewDepth)
{
    const float4 passed = (globalLightData[10] - viewDepth >= 0) ? 0 : 1;
    return dot(passed, 1);
}

// The cascade's world -> (atlas u, v, light depth) rows. Past the last split
// they are 0 (lit; the fade is 0 there anyway).
void shadow_rows(float cascade, out float4 u, out float4 v, out float4 d)
{
    u = 0, v = 0, d = 0;
    u = (cascade == 0) ? globalLightData[11] : u;
    v = (cascade == 0) ? globalLightData[12] : v;
    d = (cascade == 0) ? globalLightData[13] : d;
    u = (cascade == 1) ? globalLightData[15] : u;
    v = (cascade == 1) ? globalLightData[16] : v;
    d = (cascade == 1) ? globalLightData[17] : d;
    u = (cascade == 2) ? globalLightData[19] : u;
    v = (cascade == 2) ? globalLightData[20] : v;
    d = (cascade == 2) ? globalLightData[21] : d;
    u = (cascade == 3) ? globalLightData[23] : u;
    v = (cascade == 3) ? globalLightData[24] : v;
    d = (cascade == 3) ? globalLightData[25] : d;
}

float3 shadow_coords_in(float3 worldPos, float cascade)
{
    float4 u, v, d;
    shadow_rows(cascade, u, v, d);
    const float4 p = float4(worldPos, 1);
    return float3(dot(p, u), dot(p, v), dot(p, d));
}

// The game's lookup: the coordinates in the view depth's cascade.
float3 shadow_coords(float3 worldPos, float viewDepth)
{
    return shadow_coords_in(worldPos, shadow_cascade(viewDepth));
}

// One tap: 1 = lit (the receiver is in front of the stored depth). Projected,
// with the depth in .z as the original passes it (a hardware-compare format
// would take it as the reference). lod: an explicit level-0 read instead
// (texldl; the atlas has one level and is point-sampled, so it reads the same
// texel), for use inside a dynamic branch, where gradients aren't available.
float shadow_tap(float3 t, float2 offsetTexels, bool lod = false)
{
    const float2 uv = t.xy + offsetTexels * kShadowTexel;
    float stored;
    if (lod) {  // a compile-time constant: one of the two reads is compiled
        stored = tex2Dlod(globalDirLightShadowMap, float4(uv, t.z, 0)).x;
    } else {
        stored = tex2Dproj(globalDirLightShadowMap, float4(uv, t.z, 1)).x;
    }
    return (t.z - stored >= 0) ? 0 : 1;
}

// The receiver's footprint: how far the atlas uv moves over one screen pixel
// in x (.xy) and y (.zw), from the world position's derivatives through the
// cascade's rows. World-space derivatives keep it continuous where
// neighbouring pixels pick different cascades, and let the blend band's
// second lookup use it without derivatives inside its branch.
float4 shadow_footprint(float3 dWdx, float3 dWdy, float cascade)
{
    float4 u, v, d;
    shadow_rows(cascade, u, v, d);
    return float4(dot(dWdx, u.xyz), dot(dWdx, v.xyz), dot(dWdy, u.xyz), dot(dWdy, v.xyz));
}

// One tap, clamped to the cascade's quarter of the atlas (cascade c spans
// v = c/4 .. (c+1)/4), so a wide footprint never reads a neighbour's depths.
// The atlas is point-sampled, so the clamp only has to stay inside the
// quarter's last texel: a fixed inset below half a texel at every
// shadow_resolution (4096: 1/16384). Deriving it from kShadowTexel would let
// the compiler fold a constant the resolution rescale doesn't know
// (hooks/shader_reimpl.cpp matches exact texel values).
static const float kQuarterInset = 1.0 / 32768.0;

float shadow_tap_at(float3 t, float2 uv, float cascade, bool lod)
{
    const float2 lo = float2(0, cascade * 0.25);
    const float2 hi = float2(1, cascade * 0.25 + 0.25) - kQuarterInset;
    return shadow_tap(float3(clamp(uv, lo, hi), t.z), 0, lod);
}

// The footprint filter: past kFootprintStart texels per pixel the game's
// 2-texel tent undersamples the receiver (grazing angles: roads, terrain), and
// thin shadows (poles, fences, trunks) flicker on and off as head motion
// slides each pixel's sample across them. Over kFootprintStart ..
// kFootprintFull the 9 taps move from the tent's positions and weights to an
// even spread over the pixel's footprint (one tap per row and per column,
// "N-rooks"), each 1/9, so a thin shadow moves the result by ~1/9 instead of
// all the way. Below kFootprintStart: exactly the game's filter. Footprints
// are capped at kFootprintMax texels. Same 9 taps, no branch.
static const float kFootprintStart = 2.0;
static const float kFootprintFull = 4.0;
static const float kFootprintMax = 32.0;

// The game's 9-tap filter: a 3x3 grid of taps at texel offsets
// {-1/2, +1/2, +3/2}, averaged as four 2x2 groups, blended bilinearly by the
// position within the texel: a 2-texel tent (with the original's constant
// half-texel shift). As per-tap weights it is separable: per axis
// (1-f)/2, 1/2, f/2 at -1/2, +1/2, +3/2.
float shadow_9tap(float3 t, float4 footprint, float cascade, bool lod = false)
{
    const float2 f = frac((t.xy + 0.5 * kShadowTexel) * kShadowSize);
    const float3 wx = float3(0.5 - 0.5 * f.x, 0.5, 0.5 * f.x);
    const float3 wy = float3(0.5 - 0.5 * f.y, 0.5, 0.5 * f.y);

    // The footprint in texels, capped; the blend towards the spread filter.
    float2 a = footprint.xy, b = footprint.zw;
    const float len = max(length(a * kShadowSize), length(b * kShadowSize));
    const float cap = min(1, kFootprintMax / max(len, 1e-6));
    a *= cap, b *= cap;
    const float k = saturate((len - kFootprintStart) / (kFootprintFull - kFootprintStart));

    const float2 centre = t.xy + 0.5 * kShadowTexel;
    float s = 0;
    [unroll] for (int j = 0; j < 3; j++) {
        [unroll] for (int i = 0; i < 3; i++) {
            const int n = j * 3 + i;
            // The tent's tap (i, j) and its weight.
            const float2 tent = t.xy + (float2(i, j) - 0.5) * kShadowTexel;
            const float wt = wx[i] * wy[j];
            // The spread's tap n: row n, column (4n mod 9), centred in the footprint.
            const float2 r = (float2(n, (n * 4) % 9) + 0.5) / 9.0 - 0.5;
            const float2 spread = centre + r.x * a + r.y * b;
            s += lerp(wt, 1.0 / 9.0, k) * shadow_tap_at(t, lerp(tent, spread, k), cascade, lod);
        }
    }
    return s;
}

// The blend band: over the last kCascadeBlendBand of each cascade's depth
// range (cascades 0-2), the next cascade's lookup is faded in, reaching it at
// the boundary, so the change in texel size and filter width is a gradient
// instead of a line. The stabilized fit makes the next cascade cover the band
// (reimpl/render/fit_shadow_cascades.cpp, the same fraction: change both).
// Only pixels inside a band take the second lookup.
static const float kCascadeBlendBand = 0.2;

// The sun's shadow at a receiver (1 lit, 0 shadowed), before the fade.
float sun_shadow(float3 worldPos, float viewDepth)
{
    const float cascade = shadow_cascade(viewDepth);
    // Outside the band's branch: derivatives aren't available inside it.
    const float3 dWdx = ddx(worldPos), dWdy = ddy(worldPos);
    float s = shadow_9tap(shadow_coords_in(worldPos, cascade), shadow_footprint(dWdx, dWdy, cascade), cascade);
    // The cascade's depth range: from the previous split end (0 before the
    // first) to its own.
    const float4 pick = (cascade == float4(0, 1, 2, 3)) ? 1 : 0;
    const float end = dot(pick, globalLightData[10]);
    const float start = dot(pick.yzww * float4(1, 1, 1, 0), globalLightData[10].xyzw);
    const float width = (end - start) * kCascadeBlendBand;
    const float t = saturate((viewDepth - (end - width)) / width);
    [branch] if (cascade < 3 && t > 0) {
        s = lerp(s, shadow_9tap(shadow_coords_in(worldPos, cascade + 1),
                                shadow_footprint(dWdx, dWdy, cascade + 1), cascade + 1, true), t);
    }
    return s;
}

// The sun's visibility at a receiver: 1 lit, 0 shadowed, faded towards 1 by
// the VS's fade (1 near, 0 at the end of the shadow range).
float sun_visibility(float3 worldPos, float viewDepth, float fade)
{
    return lerp(1, sun_shadow(worldPos, viewDepth), saturate(fade));
}

#endif
