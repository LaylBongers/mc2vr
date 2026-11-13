// Local lights (point and spot), as the game's lit shader variants apply them
// (the _pl/_sl/_pl_sl variants and their _li versions, e.g. PgFastFP_pl,
// shader3.bin@0x1d6930; docs/shaders.md). Each family comes in 7 variants: no
// local lights, or point and/or spot lights in one of two counting schemes,
// chosen at build time:
//
//   MC2VR_POINT_LIGHTS  0 | 1     the variant has point lights
//   MC2VR_SPOT_LIGHTS   0 | 1     the variant has spot lights
//   MC2VR_LIGHTS_LI     0 | 1     0 = the plain variants (_pl, _sl, _pl_sl):
//                                 the highest set bool picks the count (points
//                                 8/4/2/1 by b3/b2/b1/b0, spots 4/2/1 by
//                                 b6/b5/b4); 1 = the _li variants: 2 always,
//                                 +2 if b2 (points) / b6 (spots), points +2
//                                 more if also b3.
//
// The bools are static branches (`if bN`), uploaded by the game with
// SetPixelShaderConstantB (numPointLights b0-b3, numSpotLights b4-b6).

#ifndef MC2VR_LIGHTS_HLSLI
#define MC2VR_LIGHTS_HLSLI

#ifndef MC2VR_POINT_LIGHTS
#define MC2VR_POINT_LIGHTS 0
#endif
#ifndef MC2VR_SPOT_LIGHTS
#define MC2VR_SPOT_LIGHTS 0
#endif
#ifndef MC2VR_LIGHTS_LI
#define MC2VR_LIGHTS_LI 0
#endif

#if MC2VR_POINT_LIGHTS
bool numPointLights[4] : register(b0);
// Per light: position, colour, attenuation (x scale, z bias, by distance).
#if !MC2VR_LIGHTS_LI
float4 pointLights[24] : register(c36);  // 8 lights
#else
float4 pointLights[18] : register(c36);  // 6 lights
#endif

float3 point_light(int k, float3 worldPos, float3 normal)
{
    const float3 d = worldPos - pointLights[3 * k].xyz;
    const float dist = length(d);
    const float3 l = d / dist;
    const float att = saturate(dist * pointLights[3 * k + 2].x + pointLights[3 * k + 2].z);
    return att * pointLights[3 * k + 1].rgb * max(dot(normal, -l), 0);
}
#endif

#if MC2VR_SPOT_LIGHTS
bool numSpotLights[3] : register(b4);
// Per light: position (w = the cone's clamp minimum), direction (w = its
// maximum), colour, (near, exponent, far, cone scale).
float4 spotLights[16] : register(c60);  // 4 lights

float3 spot_light(int k, float3 worldPos, float3 normal)
{
    const float4 s0 = spotLights[4 * k], s1 = spotLights[4 * k + 1];
    const float4 s2 = spotLights[4 * k + 2], s3 = spotLights[4 * k + 3];
    const float3 d = worldPos - s0.xyz;
    const float dist = length(d);
    const float3 l = d / dist;
    // abs(): both bases are >= 0 by construction, and SM3's pow takes |x| anyway.
    const float range = pow(abs((s3.z - clamp(dist, s3.x, s3.z)) * (1 / (s3.z - s3.x))), s3.y);
    const float cosA = dot(s1.xyz, l);
    const float cone = 1 - pow(abs((s1.w - clamp(cosA, s0.w, s1.w)) * s3.w), s3.y);
    const float ahead = (cosA >= 0) ? 1 : 0;
    return range * s2.rgb * cone * ahead * max(dot(normal, -l), 0);
}
#endif

// The local lights' sum at a receiver.
float3 local_lights(float3 worldPos, float3 normal)
{
    float3 sum = 0;
#if MC2VR_POINT_LIGHTS
#if !MC2VR_LIGHTS_LI
    if (numPointLights[3]) {
        [unroll] for (int k = 0; k < 8; k++) sum += point_light(k, worldPos, normal);
    } else if (numPointLights[2]) {
        [unroll] for (int k = 0; k < 4; k++) sum += point_light(k, worldPos, normal);
    } else if (numPointLights[1]) {
        [unroll] for (int k = 0; k < 2; k++) sum += point_light(k, worldPos, normal);
    } else if (numPointLights[0]) {
        sum += point_light(0, worldPos, normal);
    }
#else
    sum += point_light(0, worldPos, normal);
    sum += point_light(1, worldPos, normal);
    if (numPointLights[2]) {
        sum += point_light(2, worldPos, normal);
        sum += point_light(3, worldPos, normal);
        if (numPointLights[3]) {
            sum += point_light(4, worldPos, normal);
            sum += point_light(5, worldPos, normal);
        }
    }
#endif
#endif
#if MC2VR_SPOT_LIGHTS
#if !MC2VR_LIGHTS_LI
    if (numSpotLights[2]) {
        [unroll] for (int k = 0; k < 4; k++) sum += spot_light(k, worldPos, normal);
    } else if (numSpotLights[1]) {
        [unroll] for (int k = 0; k < 2; k++) sum += spot_light(k, worldPos, normal);
    } else if (numSpotLights[0]) {
        sum += spot_light(0, worldPos, normal);
    }
#else
    sum += spot_light(0, worldPos, normal);
    sum += spot_light(1, worldPos, normal);
    if (numSpotLights[2]) {
        sum += spot_light(2, worldPos, normal);
        sum += spot_light(3, worldPos, normal);
    }
#endif
#endif
    return sum;
}

#endif
