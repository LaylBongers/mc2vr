// PgDecal2FP (shader3.bin@0xa2a0) and its 6 light variants PgDecal2FP_pl ..
// PgDecal2FP_sl_li (up to @0x10070; docs/shaders.md) — reimplementation.
// Decals. The originals take a single binary shadow tap; these use the game's
// own 9-tap filter (shadow.hlsli). Everything else, including the interface,
// is the originals'. No material constants: ambient cube plus the sun and the
// local lights, on the interpolated normal as it comes (not normalized, as the
// originals), unsaturated. Local lights: lights.hlsli's defines select the
// variant.

#ifndef MC2VR_SHADING_MODERN
#define MC2VR_SHADING_MODERN 0  // conf shading=classic|modern: the `_modern` symbols at the modern main
#endif

#include "shadow.hlsli"
#include "lights.hlsli"

sampler2D diffuseMap : register(s0);

struct Input {
    float4 uvDepthFade : TEXCOORD0;  // uv, view depth, shadow fade
    float3 worldPos : TEXCOORD3;
    float3 normal : TEXCOORD4;
    float4 extinction : TEXCOORD5;   // atmosphere: colour * this + inscatter; w = alpha
    float3 inscatter : TEXCOORD7;
};

#if !MC2VR_SHADING_MODERN
float4 main(Input i) : COLOR
{
    const float4 albedo = tex2D(diffuseMap, i.uvDepthFade.xy);
    const float3 sun = sun_visibility(i.worldPos, i.uvDepthFade.z, i.uvDepthFade.w) * globalLightData[1].rgb;
    const float ndl = max(dot(i.normal, -globalLightData[0].xyz), 0);
    const float3 direct = sun * ndl + local_lights(i.worldPos, i.normal);
    const float3 light = ambient_cube(i.normal) + direct;
    const float3 colour = albedo.rgb * light * i.extinction.rgb + i.inscatter;
    return float4(min(max(colour, 1e-5), 30000), albedo.a * i.extinction.w);
}
#endif  // !MC2VR_SHADING_MODERN

#if MC2VR_SHADING_MODERN
// ---- The modern shading (modern.hlsli) ----
//
// A dielectric: Burley diffuse with the energy split and GGX specular from
// the sun (fully shadowed) and the local lights; the ambient cube at the
// normal through the split for the diffuse and, along the reflection vector
// and weighted by the split-sum fit, as the environment. Still no material
// constants. The BRDF needs a unit normal, so unlike the classic shading the
// normal is normalized.
#include "modern.hlsli"

float3 cameraPos : register(c92);  // not in the originals' interface: the carrier's, per eye

static const float kDecalAlpha = 0.7;        // GGX alpha
static const float kDecalEnvStrength = 1.0;  // environment specular scale

float4 main(Input i) : COLOR
{
    const float4 albedo = tex2D(diffuseMap, i.uvDepthFade.xy);
    const float3 normal = normalize(i.normal);
    const float3 view = normalize(cameraPos - i.worldPos);
    const PbrMaterial m = dielectric(kDecalAlpha, normal, view);
    const float visibility = sun_visibility(i.worldPos, i.uvDepthFade.z, i.uvDepthFade.w);
    const LocalLights direct = direct_light_pbr(i.worldPos, normal, view, m, visibility);
    const float3 environment = ambient_cube(reflect(-view, normal));
    const float3 lit = dielectric_colour(albedo.rgb, m, direct, 1, ambient_cube(normal), environment, kDecalEnvStrength);
    const float3 colour = lit * i.extinction.rgb + i.inscatter;
    return float4(min(max(colour, 1e-5), 30000), albedo.a * i.extinction.w);
}
#endif  // MC2VR_SHADING_MODERN
