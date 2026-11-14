// The sun's cascaded shadow lookup, as the game's high-quality receivers do it
// (shader3.bin@0x11570, the diffuseMap + materialData shader; docs/shadows.md),
// plus a blend band at the cascade boundaries (sun_shadow; not in the game).
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

// World position -> (atlas u, v, light depth) in the given cascade. Past the
// last split the coordinates are 0 (lit; the fade is 0 there anyway).
float3 shadow_coords_in(float3 worldPos, float cascade)
{
    const float4 p = float4(worldPos, 1);
    float4 u = 0, v = 0, d = 0;
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

// The game's 9-tap filter: a 3x3 grid of taps at texel offsets
// {-1/2, +1/2, +3/2}, averaged as four 2x2 groups, blended bilinearly by the
// position within the texel: a 2-texel tent (with the original's constant
// half-texel shift).
float shadow_9tap(float3 t, bool lod = false)
{
    const float c = shadow_tap(t, float2(0.5, 0.5), lod);
    const float4 groups = 0.25 * (
        float4(shadow_tap(t, float2(-0.5, -0.5), lod), shadow_tap(t, float2(1.5, -0.5), lod),
               shadow_tap(t, float2(-0.5, 1.5), lod), shadow_tap(t, float2(1.5, 1.5), lod)) +
        float4(shadow_tap(t, float2(0.5, -0.5), lod), shadow_tap(t, float2(0.5, -0.5), lod),
               shadow_tap(t, float2(0.5, 1.5), lod), shadow_tap(t, float2(0.5, 1.5), lod)) +
        float4(shadow_tap(t, float2(-0.5, 0.5), lod), shadow_tap(t, float2(1.5, 0.5), lod),
               shadow_tap(t, float2(-0.5, 0.5), lod), shadow_tap(t, float2(1.5, 0.5), lod)) +
        c);
    const float2 f = frac((t.xy + 0.5 * kShadowTexel) * kShadowSize);
    const float4 w = float4((1 - f.x) * (1 - f.y), f.x * (1 - f.y), (1 - f.x) * f.y, f.x * f.y);
    return dot(w, groups);
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
    float s = shadow_9tap(shadow_coords_in(worldPos, cascade));
    // The cascade's depth range: from the previous split end (0 before the
    // first) to its own.
    const float4 pick = (cascade == float4(0, 1, 2, 3)) ? 1 : 0;
    const float end = dot(pick, globalLightData[10]);
    const float start = dot(pick.yzww * float4(1, 1, 1, 0), globalLightData[10].xyzw);
    const float width = (end - start) * kCascadeBlendBand;
    const float t = saturate((viewDepth - (end - width)) / width);
    [branch] if (cascade < 3 && t > 0) {
        s = lerp(s, shadow_9tap(shadow_coords_in(worldPos, cascade + 1), true), t);
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
