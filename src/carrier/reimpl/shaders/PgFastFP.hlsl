// PgFastFP (shader3.bin@0x1d6370) and its 6 light variants PgFastFP_pl ..
// PgFastFP_sl_li (up to @0x1dc250; docs/shaders.md) — reimplementation. The
// game's cheap generic material: vegetation and other "fast" meshes. The originals take a single binary
// shadow tap; these use the game's own 9-tap filter (shadow.hlsli), as its
// other high-quality receivers do. Everything else, including the interface
// (inputs, constant registers, samplers), is the originals'. Local lights:
// lights.hlsli's defines select the variant.

#ifndef MC2VR_SHADING_MODERN
#define MC2VR_SHADING_MODERN 0  // conf shading=classic|modern: the `_modern` symbols at the modern main
#endif

#include "shadow.hlsli"
#include "lights.hlsli"

sampler2D diffuseMap : register(s0);
float4 materialData[6] : register(c93);  // [0] diffuse, [2] ambient, [5].w alpha

struct Input {
    float4 uvDepthFade : TEXCOORD0;  // uv, view depth, shadow fade
    float3 normal : TEXCOORD3;
    float3 extinction : TEXCOORD4;   // atmosphere: colour * this + inscatter
    float3 inscatter : TEXCOORD5;
    float3 worldPos : TEXCOORD6;
    float3 ambient : COLOR1;
};

#if !MC2VR_SHADING_MODERN
float4 main(Input i) : COLOR
{
    const float4 albedo = tex2D(diffuseMap, i.uvDepthFade.xy);
    const float3 sun = sun_visibility(i.worldPos, i.uvDepthFade.z, i.uvDepthFade.w) * globalLightData[1].rgb;
    const float3 normal = normalize(i.normal);
    const float ndl = max(dot(normal, -globalLightData[0].xyz), 0);
    const float3 direct = saturate(sun * ndl + local_lights(i.worldPos, normal));
    const float3 light = direct * materialData[0].rgb + materialData[2].rgb * i.ambient;
    const float3 colour = albedo.rgb * light * i.extinction + i.inscatter;
    return float4(min(max(colour, 1e-5), 30000), albedo.a * materialData[5].w);
}
#endif  // !MC2VR_SHADING_MODERN

#if MC2VR_SHADING_MODERN
// ---- The modern shading (modern.hlsli; as PgDiff.hlsl's families without Spec) ----
//
// A dielectric: Burley diffuse with the energy split and GGX specular from
// the sun (fully shadowed) and the local lights, unsaturated; the material's
// ambient (materialData[2] x the vertex's) through the split for the diffuse
// and, weighted by the split-sum fit, as the environment (no direction).
#include "modern.hlsli"

float3 cameraPos : register(c92);  // not in the originals' interface; the game uploads it by name, per eye

static const float kFastAlpha = 0.6;        // GGX alpha
static const float kFastEnvStrength = 1.0;  // environment specular scale

float4 main(Input i) : COLOR
{
    const float4 albedo = tex2D(diffuseMap, i.uvDepthFade.xy);
    const float3 normal = normalize(i.normal);
    const float3 view = normalize(cameraPos - i.worldPos);
    const PbrMaterial m = dielectric(kFastAlpha, normal, view);
    const float visibility = sun_visibility(i.worldPos, i.uvDepthFade.z, i.uvDepthFade.w);
    const LocalLights direct = direct_light_pbr(i.worldPos, normal, view, m, visibility);
    const float3 ambient = materialData[2].rgb * i.ambient;
    const float3 lit = dielectric_colour(albedo.rgb, m, direct, materialData[0].rgb, ambient, ambient, kFastEnvStrength);
    const float3 colour = lit * i.extinction + i.inscatter;
    return float4(min(max(colour, 1e-5), 30000), albedo.a * materialData[5].w);
}
#endif  // MC2VR_SHADING_MODERN
