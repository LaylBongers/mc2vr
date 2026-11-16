// The refraction families (docs/shaders.md) — reimplementation. Each is one
// bytecode for all 7 light variants:
//
//   PgDiffRefractFP      (shader3.bin@0x4aa40 .. @0x4c4e0)  the interpolated normal
//   PgDiffRefractNormFP  (shader3.bin@0x4c950 .. @0x4ea50)  MC2VR_PGDIFF_REFRACT_NORM:
//                        normalMap in the tangent frame (normal_map.hlsli)
//
// The scene behind (diffuseMap, read at the pixel's screen position) is
// shifted by the normal's world xy scaled by N·V · materialData[5].y, and
// tinted by the ambient cube, an emissive term and the sun. Not a shadow
// receiver and no local lights, in any variant. Both shadings use these.
//
// The only change from the originals: the normal map's z is reconstructed
// correctly (normal_map.hlsli; PgDiffRefractFP has no normal map and is
// exactly the original). Kept as the originals: the sun term dots the normal
// with +globalLightData[0] (the light's travel direction), where every
// PgDiff family uses -globalLightData[0]: the surface is lit from behind.
// Whether that's a slip or meant as transmission is unknown.

#ifndef MC2VR_PGDIFF_REFRACT_NORM
#define MC2VR_PGDIFF_REFRACT_NORM 0
#endif

#include "global_light.hlsli"
#if MC2VR_PGDIFF_REFRACT_NORM
#include "normal_map.hlsli"
#endif

float4 viewport : register(c0);       // zw = 1 / render size
float3 cameraPos : register(c92);
// [0] diffuse, [2] ambient, [3] emissive, [5] y refraction strength, w alpha.
float4 materialData[6] : register(c93);

sampler2D diffuseMap : register(s0);  // the scene behind
#if MC2VR_PGDIFF_REFRACT_NORM
sampler2D normalMap : register(s2);
#endif

struct Input {
#if MC2VR_PGDIFF_REFRACT_NORM
    float2 uv : TEXCOORD0;
    float3 tangent : TEXCOORD1;
    float3 binormal : TEXCOORD2;
#endif
    float3 normal : TEXCOORD3;
    float3 worldPos : TEXCOORD6;
    float2 vpos : VPOS;
};

float4 main(Input i) : COLOR
{
#if MC2VR_PGDIFF_REFRACT_NORM
    const float3 normal = mapped_normal(tex2D(normalMap, i.uv), i.tangent, i.binormal, i.normal);
#else
    const float3 normal = normalize(i.normal);
#endif
    const float3 sun = max(dot(globalLightData[0].xyz, normal), 0) * globalLightData[1].rgb;
    const float3 light = ambient_cube(normal) * materialData[2].rgb + materialData[3].rgb + sun * materialData[0].rgb;
    const float3 view = normalize(cameraPos - i.worldPos);
    const float2 offset = normal.xy * (dot(view, normal) * materialData[5].y);
    const float2 screenUv = (i.vpos + 0.5) * viewport.zw + offset;
    return float4(light * tex2D(diffuseMap, screenUv).rgb, materialData[5].w);
}
