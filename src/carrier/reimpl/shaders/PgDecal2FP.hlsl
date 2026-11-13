// PgDecal2FP (shader3.bin@0xa2a0) and its 6 light variants PgDecal2FP_pl ..
// PgDecal2FP_sl_li (up to @0x10070; docs/shaders.md) — reimplementation.
// Decals. The originals take a single binary shadow tap; these use the game's
// own 9-tap filter (shadow.hlsli). Everything else, including the interface,
// is the originals'. No material constants: ambient cube plus the sun and the
// local lights, on the interpolated normal as it comes (not normalized, as the
// originals), unsaturated. Local lights: lights.hlsli's defines select the
// variant.

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
