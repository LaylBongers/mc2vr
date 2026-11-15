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

// Light k's colour reaching the receiver, and the unit direction from the
// light to it (l).
float3 point_radiance(int k, float3 worldPos, out float3 l)
{
    const float3 d = worldPos - pointLights[3 * k].xyz;
    const float dist = length(d);
    l = d / dist;
    const float att = saturate(dist * pointLights[3 * k + 2].x + pointLights[3 * k + 2].z);
    return att * pointLights[3 * k + 1].rgb;
}
#endif

#if MC2VR_SPOT_LIGHTS
bool numSpotLights[3] : register(b4);
// Per light: position (w = the cone's clamp minimum), direction (w = its
// maximum), colour, (near, exponent, far, cone scale).
float4 spotLights[16] : register(c60);  // 4 lights

float3 spot_radiance(int k, float3 worldPos, out float3 l)
{
    const float4 s0 = spotLights[4 * k], s1 = spotLights[4 * k + 1];
    const float4 s2 = spotLights[4 * k + 2], s3 = spotLights[4 * k + 3];
    const float3 d = worldPos - s0.xyz;
    const float dist = length(d);
    l = d / dist;
    // abs(): both bases are >= 0 by construction, and SM3's pow takes |x| anyway.
    const float range = pow(abs((s3.z - clamp(dist, s3.x, s3.z)) * (1 / (s3.z - s3.x))), s3.y);
    const float cosA = dot(s1.xyz, l);
    const float cone = 1 - pow(abs((s1.w - clamp(cosA, s0.w, s1.w)) * s3.w), s3.y);
    const float ahead = (cosA >= 0) ? 1 : 0;
    return range * s2.rgb * cone * ahead;
}
#endif

// Blinn-Phong: (N.H)^power towards a light (unit direction toLight), from a
// viewer (unit direction view); zero where the receiver faces away from the
// light or from the half vector (the PgDiff* families, e.g.
// PgDiffSpecNormFP_pl, shader3.bin@0xf27f0).
float blinn_phong(float3 normal, float3 toLight, float3 view, float power)
{
    const float ndl = dot(normal, toLight);
    const float ndh = dot(normalize(view + toLight), normal);
    // abs(): masked to ndh > 0 anyway, and SM3's pow takes |x|.
    return ((ndl > 0) ? 1 : 0) * ((ndh > 0) ? 1 : 0) * pow(abs(ndh), power);
}

// The local lights at a receiver: diffuse (radiance * N.L) and Blinn-Phong
// specular (radiance * blinn_phong), each summed over the variant's lights.
struct LocalLights {
    float3 diffuse;
    float3 specular;
};

void add_light(inout LocalLights sum, float3 radiance, float3 l, float3 normal, float3 view, float power)
{
    sum.diffuse += radiance * max(dot(normal, -l), 0);
    sum.specular += radiance * blinn_phong(normal, -l, view, power);
}

#if MC2VR_POINT_LIGHTS
void add_point(inout LocalLights sum, int k, float3 worldPos, float3 normal, float3 view, float power)
{
    float3 l;
    const float3 radiance = point_radiance(k, worldPos, l);
    add_light(sum, radiance, l, normal, view, power);
}
#endif

#if MC2VR_SPOT_LIGHTS
void add_spot(inout LocalLights sum, int k, float3 worldPos, float3 normal, float3 view, float power)
{
    float3 l;
    const float3 radiance = spot_radiance(k, worldPos, l);
    add_light(sum, radiance, l, normal, view, power);
}
#endif

// The variant's light traversal: ADD(k) for each of its point lights
// (MC2VR_FOR_POINT_LIGHTS) and spot lights (MC2VR_FOR_SPOT_LIGHTS), in the
// originals' bool branches. ADD is a function-like macro of the light index.
#if MC2VR_POINT_LIGHTS && !MC2VR_LIGHTS_LI
#define MC2VR_FOR_POINT_LIGHTS(ADD) \
    if (numPointLights[3]) { \
        [unroll] for (int k = 0; k < 8; k++) ADD(k); \
    } else if (numPointLights[2]) { \
        [unroll] for (int k = 0; k < 4; k++) ADD(k); \
    } else if (numPointLights[1]) { \
        [unroll] for (int k = 0; k < 2; k++) ADD(k); \
    } else if (numPointLights[0]) { \
        ADD(0); \
    }
#elif MC2VR_POINT_LIGHTS
#define MC2VR_FOR_POINT_LIGHTS(ADD) \
    ADD(0); \
    ADD(1); \
    if (numPointLights[2]) { \
        ADD(2); \
        ADD(3); \
        if (numPointLights[3]) { \
            ADD(4); \
            ADD(5); \
        } \
    }
#else
#define MC2VR_FOR_POINT_LIGHTS(ADD)
#endif
#if MC2VR_SPOT_LIGHTS && !MC2VR_LIGHTS_LI
#define MC2VR_FOR_SPOT_LIGHTS(ADD) \
    if (numSpotLights[2]) { \
        [unroll] for (int k = 0; k < 4; k++) ADD(k); \
    } else if (numSpotLights[1]) { \
        [unroll] for (int k = 0; k < 2; k++) ADD(k); \
    } else if (numSpotLights[0]) { \
        ADD(0); \
    }
#elif MC2VR_SPOT_LIGHTS
#define MC2VR_FOR_SPOT_LIGHTS(ADD) \
    ADD(0); \
    ADD(1); \
    if (numSpotLights[2]) { \
        ADD(2); \
        ADD(3); \
    }
#else
#define MC2VR_FOR_SPOT_LIGHTS(ADD)
#endif

// The variant's local lights, diffuse and specular, at a receiver seen from
// view (unit direction) with the specular exponent power.
LocalLights local_lights_spec(float3 worldPos, float3 normal, float3 view, float power)
{
    LocalLights sum;
    sum.diffuse = 0;
    sum.specular = 0;
#define MC2VR_ADD_POINT_SPEC(k) add_point(sum, k, worldPos, normal, view, power)
#define MC2VR_ADD_SPOT_SPEC(k) add_spot(sum, k, worldPos, normal, view, power)
    MC2VR_FOR_POINT_LIGHTS(MC2VR_ADD_POINT_SPEC)
    MC2VR_FOR_SPOT_LIGHTS(MC2VR_ADD_SPOT_SPEC)
    return sum;
}

// The local lights' diffuse sum at a receiver (the families without
// specular; the specular terms are dead code there).
float3 local_lights(float3 worldPos, float3 normal)
{
    return local_lights_spec(worldPos, normal, 0, 1).diffuse;
}

// ---- Physically based shading (not in the game; PgDiff.hlsl's modern model) ----
//
// Per light of radiance E from unit direction toLight, in the game's light
// units (its Lambert is albedo * E * N.L, no 1/pi), so both lobes carry a
// factor pi:
//   specular = pi * D * V * F * N.L * E    (GGX / Trowbridge-Reitz D, Smith-GGX
//              height-correlated V, Schlick F at L.H)
//   diffuse  = pi * Burley * (1 - F) * N.L * E   (Disney diffuse; the albedo is
//              applied by the caller)

// The surface as the BRDF sees it.
struct PbrMaterial {
    float3 f0;         // specular reflectance at normal incidence
    float alpha;       // GGX alpha (= roughness²)
    float roughness;   // perceptual roughness (sqrt(alpha))
    float ndv;         // |N.V| (+ epsilon): the view side, shared by all lights
};

float pow5(float x)
{
    const float x2 = x * x;
    return x2 * x2 * x;
}

float3 fresnel_schlick(float3 f0, float cosine)
{
    return f0 + (1 - f0) * pow5(1 - cosine);
}

void add_pbr_light(inout LocalLights sum, float3 radiance, float3 toLight, float3 normal, float3 view,
                   PbrMaterial m)
{
    const float ndl = saturate(dot(normal, toLight));
    const float3 h = view + toLight;
    const float3 halfway = h * rsqrt(max(dot(h, h), 1e-12));
    const float ndh = saturate(dot(normal, halfway));
    const float ldh = saturate(dot(toLight, halfway));
    const float a2 = m.alpha * m.alpha;
    // pi * D: a² / (N.H² (a² - 1) + 1)²
    const float d = ndh * ndh * (a2 - 1) + 1;
    const float piD = a2 / (d * d);
    // Height-correlated Smith-GGX, as the visibility term V = G / (4 N.L N.V).
    const float vis = 0.5 / (ndl * sqrt(m.ndv * m.ndv * (1 - a2) + a2) + m.ndv * sqrt(ndl * ndl * (1 - a2) + a2));
    const float3 f = fresnel_schlick(m.f0, ldh);
    sum.specular += radiance * (ndl * piD * vis) * f;
    // pi * Burley: (1 + (f90 - 1)(1 - N.L)^5)(1 + (f90 - 1)(1 - N.V)^5)
    const float f90 = 0.5 + 2 * m.roughness * ldh * ldh;
    const float burley = (1 + (f90 - 1) * pow5(1 - ndl)) * (1 + (f90 - 1) * pow5(1 - m.ndv));
    sum.diffuse += radiance * (ndl * burley) * (1 - f);
}

#if MC2VR_POINT_LIGHTS
void add_point_pbr(inout LocalLights sum, int k, float3 worldPos, float3 normal, float3 view, PbrMaterial m)
{
    float3 l;
    const float3 radiance = point_radiance(k, worldPos, l);
    add_pbr_light(sum, radiance, -l, normal, view, m);
}
#endif

#if MC2VR_SPOT_LIGHTS
void add_spot_pbr(inout LocalLights sum, int k, float3 worldPos, float3 normal, float3 view, PbrMaterial m)
{
    float3 l;
    const float3 radiance = spot_radiance(k, worldPos, l);
    add_pbr_light(sum, radiance, -l, normal, view, m);
}
#endif

// The variant's local lights through the BRDF: diffuse (without the albedo)
// and specular, each summed over the variant's lights.
LocalLights local_lights_pbr(float3 worldPos, float3 normal, float3 view, PbrMaterial m)
{
    LocalLights sum;
    sum.diffuse = 0;
    sum.specular = 0;
#define MC2VR_ADD_POINT_PBR(k) add_point_pbr(sum, k, worldPos, normal, view, m)
#define MC2VR_ADD_SPOT_PBR(k) add_spot_pbr(sum, k, worldPos, normal, view, m)
    MC2VR_FOR_POINT_LIGHTS(MC2VR_ADD_POINT_PBR)
    MC2VR_FOR_SPOT_LIGHTS(MC2VR_ADD_SPOT_PBR)
    return sum;
}

// Karis's analytic fit of the split-sum environment BRDF (Unreal, "Physically
// Based Shading on Mobile", 2014): the specular reflectance integrated over
// the hemisphere, for an environment's light along the reflection vector.
float3 env_brdf_approx(float3 f0, float roughness, float ndv)
{
    const float4 c0 = float4(-1, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1, 0.0425, 1.04, -0.04);
    const float4 r = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28 * ndv)) * r.x + r.y;
    const float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

#endif
