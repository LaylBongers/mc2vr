// PgUnderwaterFP (shader3.bin@0x24f4d0; docs/shaders.md) — reimplementation.
// The water surface seen from below. The original takes a single binary
// shadow tap; this one uses the game's own 9-tap filter (shadow.hlsli).
// Everything else, including the interface, is the original's.

#include "shadow.hlsli"

sampler2D RefractionMap : register(s0);
sampler2D WakeNormalMap : register(s1);
float3 cameraPos : register(c92);
float4 materialData[6] : register(c93);  // [0] diffuse, [1] specular, [2] ambient (w = Fresnel base), [5].z specular power

struct Input {
    float3 normal0 : NORMAL0;        // the surface normal is cross(normal1, normal0)
    float3 normal1 : NORMAL1;
    float3 worldPos : TEXCOORD2;
    float4 refraction : TEXCOORD3;   // projected refraction coordinates (z also scales the offset)
    float4 extinction : TEXCOORD4;   // atmosphere: colour * this + inscatter; w = wake u
    float4 inscatter : TEXCOORD5;    // w = wake v
    float2 depthFade : TEXCOORD6;    // view depth, shadow fade
    float3 ambient : COLOR0;
};

float4 main(Input i) : COLOR
{
    // The wake mask: only where its alpha exceeds 1/2.
    const float4 wake = tex2Dproj(WakeNormalMap, float4(i.extinction.w, i.inscatter.w, 0, 1));
    const float cut = 0.5 - wake.a;
    clip((cut >= 0) ? -1 : 0);

    const float vis = sun_visibility(i.worldPos, i.depthFade.x, i.depthFade.y);
    const float3 sun = vis * globalLightData[1].rgb;
    const float3 specularLight = max(0.25, vis) * globalLightData[1].rgb;
    const float3 n = normalize(cross(i.normal1, i.normal0));
    const float facing = dot(n, -globalLightData[0].xyz);
    const float lit = (facing > 0) ? 1 : 0;
    const float3 diffuse = materialData[0].rgb * (sun * (facing * lit)) + materialData[2].rgb * i.ambient;

    const float3 toEye = cameraPos - i.worldPos;
    const float3 v = toEye * rsqrt(dot(toEye, toEye));
    const float3 h = normalize(v - globalLightData[0].xyz);
    const float ndh = dot(h, n);
    const float specular = pow(abs(ndh), materialData[5].z) * (lit * ((ndh > 0) ? 1 : 0));
    const float3 surface = specularLight * specular * materialData[1].rgb + diffuse;

    const float3 r = reflect(-v, n);
    const float f = 1 - max(dot(n, r), 0);
    const float fresnel = lerp(materialData[2].w, 1, f * ((f * f) * (f * f)));
    const float4 refracted = tex2Dproj(RefractionMap, float4(i.refraction.xy - n.xz * i.refraction.z, i.refraction.zw));
    const float3 water = lerp(refracted.rgb, surface, fresnel * 0.8);
    const float3 colour = min(max(water * i.extinction.rgb + i.inscatter.rgb, 1e-5), 30000);
    return (cut >= 0) ? float4(0, 0, 0, 0) : float4(colour, 1);
}
