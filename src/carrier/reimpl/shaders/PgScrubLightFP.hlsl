// PgScrubLightFP (shader3.bin@0x2004c0) and its 6 light variants
// PgScrubLightFP_pl .. PgScrubLightFP_sl_li (up to @0x206340; docs/shaders.md)
// — reimplementation. Scrub/brush vegetation. Same change as PgFastFP.hlsl: the game's 9-tap shadow filter (shadow.hlsli) instead
// of the originals' single binary tap. Everything else, including the
// interface, is the originals'. Unlike the near family it neither normalizes
// the normal nor saturates the direct light (as the originals). Local lights:
// lights.hlsli's defines select the variant.

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
