// PgScrubLightFP (shader3.bin@0x2004c0) and its 6 light variants
// PgScrubLightFP_pl .. PgScrubLightFP_sl_li (up to @0x206340; docs/shaders.md)
// — reimplementation. Scrub/brush vegetation. Same change as PgFastFP.hlsl: the game's 9-tap shadow filter (shadow.hlsli) instead
// of the originals' single binary tap. Everything else, including the
// interface, is the originals'. Unlike the near family it neither normalizes
// the normal nor saturates the direct light (as the originals). Local lights:
// lights.hlsli's defines select the variant.

#ifndef MC2VR_SHADING_MODERN
#define MC2VR_SHADING_MODERN 0  // conf shading=classic|modern: the `_modern` symbols at the modern main
#endif

#include "shadow.hlsli"
#include "lights.hlsli"

sampler2D diffuseMap : register(s0);
float4 materialData[6] : register(c93);  // [0] diffuse, [2] ambient, [5].w alpha

struct Input {
    float4 tint : COLOR0;            // ambient tint (rgb), alpha
    float4 uvDepthFade : TEXCOORD0;  // uv, view depth, shadow fade
    float3 worldPos : TEXCOORD2;
    float3 normal : TEXCOORD3;
    float3 extinction : TEXCOORD5;   // atmosphere: colour * this + inscatter
    float3 inscatter : TEXCOORD6;
};

#if !MC2VR_SHADING_MODERN
float4 main(Input i) : COLOR
{
    const float3 sun = sun_visibility(i.worldPos, i.uvDepthFade.z, i.uvDepthFade.w) * globalLightData[1].rgb;
    const float ndl = max(dot(i.normal, -globalLightData[0].xyz), 0);
    const float3 direct = sun * ndl + local_lights(i.worldPos, i.normal);
    const float3 light = direct * materialData[0].rgb + materialData[2].rgb * i.tint.rgb;
    const float4 albedo = tex2D(diffuseMap, i.uvDepthFade.xy);
    const float3 colour = albedo.rgb * light * i.extinction + i.inscatter;
    return float4(min(max(colour, 1e-5), 30000), albedo.a * i.tint.a * materialData[5].w);
}
#endif  // !MC2VR_SHADING_MODERN

#if MC2VR_SHADING_MODERN
// ---- The modern shading (modern.hlsli; as PgFastFP.hlsl's) ----
//
// The same dielectric as PgFastFP's modern shading, with this family's
// ambient (materialData[2] x the vertex tint). The BRDF needs a unit normal,
// so unlike the classic shading the normal is normalized.
#include "modern.hlsli"

float3 cameraPos : register(c92);  // not in the originals' interface: the carrier's, per eye

static const float kScrubAlpha = 0.6;        // GGX alpha
static const float kScrubEnvStrength = 1.0;  // environment specular scale

float4 main(Input i) : COLOR
{
    const float3 normal = normalize(i.normal);
    const float3 view = normalize(cameraPos - i.worldPos);
    const PbrMaterial m = dielectric(kScrubAlpha, normal, view);
    const float visibility = sun_visibility(i.worldPos, i.uvDepthFade.z, i.uvDepthFade.w);
    const LocalLights direct = direct_light_pbr(i.worldPos, normal, view, m, visibility);
    const float3 ambient = materialData[2].rgb * i.tint.rgb;
    const float4 albedo = tex2D(diffuseMap, i.uvDepthFade.xy);
    const float3 lit = dielectric_colour(albedo.rgb, m, direct, materialData[0].rgb, ambient, ambient, kScrubEnvStrength);
    const float3 colour = lit * i.extinction + i.inscatter;
    return float4(min(max(colour, 1e-5), 30000), albedo.a * i.tint.a * materialData[5].w);
}
#endif  // MC2VR_SHADING_MODERN
