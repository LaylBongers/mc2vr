// PgFastFP (shader3.bin@0x1d6370) and its 6 light variants PgFastFP_pl ..
// PgFastFP_sl_li (up to @0x1dc250; docs/shaders.md) — reimplementation. The
// game's cheap generic material: vegetation and other "fast" meshes. The originals take a single binary
// shadow tap; these use the game's own 9-tap filter (shadow.hlsli), as its
// other high-quality receivers do. Everything else, including the interface
// (inputs, constant registers, samplers), is the originals'. Local lights:
// lights.hlsli's defines select the variant.

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
