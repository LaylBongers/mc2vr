// PgBillboardTreeFP and PgBillboardTreeFP_fade (shader3.bin@0x2ff0 and
// @0x3790, one bytecode; docs/shaders.md) — reimplementation. Billboard
// trees: two billboard views blended, each with its own normal-map lookup
// rotated into the world. The original takes a single binary shadow tap; this
// one uses the game's own 9-tap filter (shadow.hlsli). Everything else,
// including the interface, is the original's.

#ifndef MC2VR_SHADING_MODERN
#define MC2VR_SHADING_MODERN 0  // conf shading=classic|modern: the `_modern` symbols at the modern main
#endif

#include "shadow.hlsli"

sampler2D gDiffuseTexture : register(s0);
sampler2D gNormalmap : register(s2);
float4 materialData[3] : register(c93);  // [0] diffuse, [2] ambient

struct Input {
    float4 view0 : TEXCOORD0;       // x sin, z cos of view 0's rotation about Y; w = the views' blend
    float4 view1 : TEXCOORD1;       // x sin, z cos of view 1's rotation; w = view depth
    float4 uv : TEXCOORD2;          // view 0 (xy), view 1 (zw)
    float4 extinction : TEXCOORD3;  // atmosphere: colour * this + inscatter; w = alpha
    float4 inscatter : TEXCOORD4;   // w = shadow fade
    float3 worldPos : TEXCOORD5;
};

// A billboard view's tangent-space normal, rotated about Y into the world.
float3 view_normal(float2 uv, float sinA, float cosA)
{
    const float3 n = tex2D(gNormalmap, uv).xyz * 2 - 1;
    return float3(n.x * cosA + n.z * sinA, n.y, n.z * cosA - n.x * sinA);
}

#if !MC2VR_SHADING_MODERN
float4 main(Input i) : COLOR
{
    const float3 n0 = view_normal(i.uv.xy, i.view0.x, i.view0.z);
    const float3 n1 = view_normal(i.uv.zw, i.view1.x, i.view1.z);
    const float3 normal = normalize(lerp(n0, n1, i.view0.w));
    const float3 ambient = ambient_cube(normal) * materialData[2].rgb;
    const float ndl = max(dot(normal, -globalLightData[0].xyz), 0);
    const float3 sun = sun_visibility(i.worldPos, i.view1.w, i.inscatter.w) * globalLightData[1].rgb * ndl;
    const float3 light = sun * materialData[0].rgb + ambient;
    const float4 albedo = lerp(tex2D(gDiffuseTexture, i.uv.xy), tex2D(gDiffuseTexture, i.uv.zw), i.view0.w);
    const float3 colour = light * albedo.rgb * i.extinction.rgb + i.inscatter.rgb;
    return float4(min(max(colour, 1e-5), 30000), albedo.a * i.extinction.w);
}
#endif  // !MC2VR_SHADING_MODERN

#if MC2VR_SHADING_MODERN
// ---- The modern shading (modern.hlsli) ----
//
// A dielectric on the blended billboard normal: Burley diffuse with the
// energy split and GGX specular from the sun (fully shadowed; the family has
// no local lights); the material's ambient (the ambient cube x
// materialData[2]) at the normal through the split for the diffuse and,
// along the reflection vector and weighted by the split-sum fit, as the
// environment.
#include "modern.hlsli"

float3 cameraPos : register(c92);  // not in the original's interface: the carrier's, per eye

static const float kTreeAlpha = 0.6;        // GGX alpha
static const float kTreeEnvStrength = 1.0;  // environment specular scale

float4 main(Input i) : COLOR
{
    const float3 n0 = view_normal(i.uv.xy, i.view0.x, i.view0.z);
    const float3 n1 = view_normal(i.uv.zw, i.view1.x, i.view1.z);
    const float3 normal = normalize(lerp(n0, n1, i.view0.w));
    const float3 view = normalize(cameraPos - i.worldPos);
    const PbrMaterial m = dielectric(kTreeAlpha, normal, view);
    const float visibility = sun_visibility(i.worldPos, i.view1.w, i.inscatter.w);
    const LocalLights direct = direct_light_pbr(i.worldPos, normal, view, m, visibility);
    const float3 ambient = ambient_cube(normal) * materialData[2].rgb;
    const float3 environment = ambient_cube(reflect(-view, normal)) * materialData[2].rgb;
    const float4 albedo = lerp(tex2D(gDiffuseTexture, i.uv.xy), tex2D(gDiffuseTexture, i.uv.zw), i.view0.w);
    const float3 lit = dielectric_colour(albedo.rgb, m, direct, materialData[0].rgb, ambient, environment, kTreeEnvStrength);
    const float3 colour = lit * i.extinction.rgb + i.inscatter.rgb;
    return float4(min(max(colour, 1e-5), 30000), albedo.a * i.extinction.w);
}
#endif  // MC2VR_SHADING_MODERN
