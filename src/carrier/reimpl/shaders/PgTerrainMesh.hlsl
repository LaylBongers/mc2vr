// The terrain mesh shaders (docs/shaders.md) — reimplementation. One source,
// built per family with the layer count and, like the other families, in 7
// light variants (lights.hlsli's defines):
//
//   PgTerrainMeshFP    (shader3.bin@0x2439e0 ..)  MC2VR_TERRAIN_LAYERS=0
//   PgTerrainMeshFP1D  (shader3.bin@0x222850 ..)  MC2VR_TERRAIN_LAYERS=1
//   PgTerrainMeshFP2D  (shader3.bin@0x22a8d0 ..)  MC2VR_TERRAIN_LAYERS=2
//   PgTerrainMeshFP3D  (shader3.bin@0x232be0 ..)  MC2VR_TERRAIN_LAYERS=3
//   PgTerrainMeshFP4D  (shader3.bin@0x23b190 ..)  MC2VR_TERRAIN_LAYERS=4
//
//   MC2VR_TERRAIN_LAYERS  0: the global (colour) map alone; 1..4: the global
//                         map, a detail map and that many diffuse layers,
//                         each weighted per vertex and summed
//
// The originals already take the game's 9-tap sun-shadow filter: shadow.hlsli
// is theirs, so the math is the originals' throughout. Owning the lookup is
// the point (the cascade blend band, docs/shadows.md).
//
// MC2VR_SHADING_MODERN 0 | 1 (conf shading=classic|modern): 0 the originals'
// shading; 1 the physically based one (the `_modern` symbols; at the modern
// main below).

#ifndef MC2VR_SHADING_MODERN
#define MC2VR_SHADING_MODERN 0
#endif
#ifndef MC2VR_TERRAIN_LAYERS
#define MC2VR_TERRAIN_LAYERS 0
#endif

#include "shadow.hlsli"
#include "lights.hlsli"

sampler2D GlobalMap : register(s0);
#if MC2VR_TERRAIN_LAYERS >= 1
sampler2D DetailMap : register(s1);
// The layers' diffuse maps sit on the even samplers from s2 (the odd ones
// between are unused here).
sampler2D Layer1Map_Diff : register(s2);
#endif
#if MC2VR_TERRAIN_LAYERS >= 2
sampler2D Layer2Map_Diff : register(s4);
#endif
#if MC2VR_TERRAIN_LAYERS >= 3
sampler2D Layer3Map_Diff : register(s6);
#endif
#if MC2VR_TERRAIN_LAYERS >= 4
sampler2D Layer4Map_Diff : register(s8);
#endif

struct Input {
#if MC2VR_TERRAIN_LAYERS == 0
    float2 uv : TEXCOORD0;               // global map uv
#else
    float4 uv : TEXCOORD0;               // global map uv, its weight, the detail's weight
#endif
    float3 ambient : TEXCOORD1;          // the vertex's ambient light
    float4 normalFade : TEXCOORD3;       // normal, shadow fade
#if MC2VR_TERRAIN_LAYERS == 0
    float3 extinction : TEXCOORD4;       // atmosphere: colour * this + inscatter
    float3 inscatter : TEXCOORD5;
#else
    float4 extinction : TEXCOORD4;       // w: the detail map's u
    float4 inscatter : TEXCOORD5;        // w: the detail map's v
#endif
    float4 worldPosDepth : TEXCOORD6;    // world position, view depth
#if MC2VR_TERRAIN_LAYERS >= 1
    float4 layer1 : COLOR0;              // per layer: uv, (unused), weight
#endif
#if MC2VR_TERRAIN_LAYERS >= 2
    float4 layer2 : COLOR1;
#endif
#if MC2VR_TERRAIN_LAYERS >= 3
    float4 layer3 : COLOR2;
#endif
#if MC2VR_TERRAIN_LAYERS >= 4
    float4 layer4 : COLOR3;
#endif
};

// The ground colour: the global map, or (layered) the weighted global map,
// plus a signed detail term, plus each layer's map by its weight.
float3 terrain_albedo(Input i)
{
#if MC2VR_TERRAIN_LAYERS == 0
    return tex2D(GlobalMap, i.uv).rgb;
#else
    const float2 detailUv = float2(i.extinction.w, i.inscatter.w);
    float3 albedo = 2 * ((tex2D(DetailMap, detailUv).rgb - 0.5) * i.uv.w);
    albedo += tex2D(GlobalMap, i.uv.xy).rgb * i.uv.z;
    albedo += tex2D(Layer1Map_Diff, i.layer1.xy).rgb * i.layer1.w;
#if MC2VR_TERRAIN_LAYERS >= 2
    albedo += tex2D(Layer2Map_Diff, i.layer2.xy).rgb * i.layer2.w;
#endif
#if MC2VR_TERRAIN_LAYERS >= 3
    albedo += tex2D(Layer3Map_Diff, i.layer3.xy).rgb * i.layer3.w;
#endif
#if MC2VR_TERRAIN_LAYERS >= 4
    albedo += tex2D(Layer4Map_Diff, i.layer4.xy).rgb * i.layer4.w;
#endif
    return albedo;
#endif
}

#if !MC2VR_SHADING_MODERN
float4 main(Input i) : COLOR
{
    const float3 worldPos = i.worldPosDepth.xyz;
    const float3 normal = normalize(i.normalFade.xyz);

    // The sun by its visibility, the local lights and the vertex ambient,
    // unsaturated.
    const float visibility = sun_visibility(worldPos, i.worldPosDepth.w, i.normalFade.w);
    const float3 sun = visibility * globalLightData[1].rgb * max(dot(normal, -globalLightData[0].xyz), 0);
    const float3 light = sun + local_lights(worldPos, normal) + i.ambient;

    const float3 colour = light * terrain_albedo(i) * i.extinction.xyz + i.inscatter.xyz;
    return float4(min(max(colour, 1e-5), 30000), 1);
}
#endif  // !MC2VR_SHADING_MODERN

#if MC2VR_SHADING_MODERN
// ---- The modern shading (modern.hlsli; as PgDiff.hlsl's) ----
//
// A rough dielectric: Burley diffuse with the energy split and GGX specular
// from the sun (fully shadowed) and the local lights; the vertex ambient
// through the split for the diffuse and, weighted by the split-sum fit, as
// the environment (it has no direction), at the neutral strength like every
// other family. (Half strength, set up front against wet-looking distant
// ground, made the terrain look about as dull as classic: live, 2026-11-15.)
#include "modern.hlsli"

float3 cameraPos : register(c92);  // not in the originals' interface: the carrier's, per eye

static const float kTerrainAlpha = 0.8;        // GGX alpha of the ground
static const float kTerrainEnvStrength = 1.0;  // environment specular scale (1 = neutral)

float4 main(Input i) : COLOR
{
    const float3 worldPos = i.worldPosDepth.xyz;
    const float3 normal = normalize(i.normalFade.xyz);
    const float3 view = normalize(cameraPos - worldPos);
    const PbrMaterial m = dielectric(kTerrainAlpha, normal, view);

    const float visibility = sun_visibility(worldPos, i.worldPosDepth.w, i.normalFade.w);
    const LocalLights direct = direct_light_pbr(worldPos, normal, view, m, visibility);
    const float3 lit = dielectric_colour(terrain_albedo(i), m, direct, 1, i.ambient, i.ambient, kTerrainEnvStrength);

    const float3 colour = lit * i.extinction.xyz + i.inscatter.xyz;
    return float4(min(max(colour, 1e-5), 30000), 1);
}
#endif  // MC2VR_SHADING_MODERN
