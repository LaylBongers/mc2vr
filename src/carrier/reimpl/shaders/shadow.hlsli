// The sun's cascaded shadow lookup, as the game's high-quality receivers do it
// (shader3.bin@0x11570, the diffuseMap + materialData shader; docs/shadows.md).
// Shared by the reimplemented receivers in this directory.
// globalLightData's layout: global_light.hlsli.

#ifndef MC2VR_SHADOW_HLSLI
#define MC2VR_SHADOW_HLSLI

#include "global_light.hlsli"

sampler2D globalDirLightShadowMap : register(s15);  // 1024x4096 depth atlas, POINT

// The atlas texel (four 1024² cascades stacked vertically).
static const float2 kShadowTexel = float2(1.0 / 1024.0, 1.0 / 4096.0);
static const float2 kShadowSize = float2(1024.0, 4096.0);

// World position -> (atlas u, v, light depth) in the cascade the view depth
// falls in: as many cascades are skipped as split ends lie before it. Past
// the last split the coordinates are 0 (lit; the fade is 0 there anyway).
float3 shadow_coords(float3 worldPos, float viewDepth)
{
    const float4 passed = (globalLightData[10] - viewDepth >= 0) ? 0 : 1;
    const float cascade = dot(passed, 1);
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

// One tap: 1 = lit (the receiver is in front of the stored depth). Projected,
// with the depth in .z as the original passes it (a hardware-compare format
// would take it as the reference).
float shadow_tap(float3 t, float2 offsetTexels)
{
    const float stored = tex2Dproj(globalDirLightShadowMap, float4(t.xy + offsetTexels * kShadowTexel, t.z, 1)).x;
    return (t.z - stored >= 0) ? 0 : 1;
}

// The game's 9-tap filter: a 3x3 grid of taps at texel offsets
// {-1/2, +1/2, +3/2}, averaged as four 2x2 groups, blended bilinearly by the
// position within the texel: a 2-texel tent (with the original's constant
// half-texel shift).
float shadow_9tap(float3 t)
{
    const float c = shadow_tap(t, float2(0.5, 0.5));
    const float4 groups = 0.25 * (
        float4(shadow_tap(t, float2(-0.5, -0.5)), shadow_tap(t, float2(1.5, -0.5)),
               shadow_tap(t, float2(-0.5, 1.5)), shadow_tap(t, float2(1.5, 1.5))) +
        float4(shadow_tap(t, float2(0.5, -0.5)), shadow_tap(t, float2(0.5, -0.5)),
               shadow_tap(t, float2(0.5, 1.5)), shadow_tap(t, float2(0.5, 1.5))) +
        float4(shadow_tap(t, float2(-0.5, 0.5)), shadow_tap(t, float2(1.5, 0.5)),
               shadow_tap(t, float2(-0.5, 0.5)), shadow_tap(t, float2(1.5, 0.5))) +
        c);
    const float2 f = frac((t.xy + 0.5 * kShadowTexel) * kShadowSize);
    const float4 w = float4((1 - f.x) * (1 - f.y), f.x * (1 - f.y), (1 - f.x) * f.y, f.x * f.y);
    return dot(w, groups);
}

// The sun's visibility at a receiver: 1 lit, 0 shadowed, faded towards 1 by
// the VS's fade (1 near, 0 at the end of the shadow range).
float sun_visibility(float3 worldPos, float viewDepth, float fade)
{
    return lerp(1, shadow_9tap(shadow_coords(worldPos, viewDepth)), saturate(fade));
}

#endif
