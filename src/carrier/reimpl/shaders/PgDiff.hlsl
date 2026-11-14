// The PgDiff* material families (the game's high-quality "PgDiff<features>FP"
// shaders; docs/shaders.md) — reimplementation. One source, built per family
// with feature defines and, like the other families, in 7 light variants
// (lights.hlsli's defines):
//
//   PgDiffSpecNormAmbOccFP  (shader3.bin@0xd9c70 ..)  SPEC NORM AMBOCC
//   PgDiffSpecNormFP        (shader3.bin@0xf1bf0 ..)  SPEC NORM
//   PgDiffSpecMetalNormFP   (shader3.bin@0xb5270 ..)  SPEC NORM METAL
//
//   MC2VR_PGDIFF_SPEC    specularMap: Blinn-Phong specular from the sun and the
//                        local lights, its luminance raising the alpha
//   MC2VR_PGDIFF_NORM    normalMap: a two-channel tangent-space normal
//   MC2VR_PGDIFF_AMBOCC  the vertex colour (COLOR0) multiplies the lit colour
//                        and the alpha
//   MC2VR_PGDIFF_METAL   reflectionMap: a cube reflection, tinted by the
//                        specular map, weighted by a Fresnel term
//
// Unlike our other receivers' originals, these already take the game's 9-tap
// sun-shadow filter: shadow.hlsli is theirs, so the math is the originals'
// throughout. Owning the lookup is the point (a cascade blend band, docs/shadows.md).
// Combinations without SPEC and NORM have not been transcribed yet.

#ifndef MC2VR_PGDIFF_SPEC
#define MC2VR_PGDIFF_SPEC 0
#endif
#ifndef MC2VR_PGDIFF_NORM
#define MC2VR_PGDIFF_NORM 0
#endif
#ifndef MC2VR_PGDIFF_AMBOCC
#define MC2VR_PGDIFF_AMBOCC 0
#endif
#ifndef MC2VR_PGDIFF_METAL
#define MC2VR_PGDIFF_METAL 0
#endif

#if !MC2VR_PGDIFF_SPEC || !MC2VR_PGDIFF_NORM
#error "PgDiff.hlsl: only the Spec+Norm families are transcribed so far"
#endif

#include "shadow.hlsli"
#include "lights.hlsli"

sampler2D diffuseMap : register(s0);
sampler2D specularMap : register(s1);
sampler2D normalMap : register(s2);
#if MC2VR_PGDIFF_METAL
samplerCUBE reflectionMap : register(s13);
#endif

float3 cameraPos : register(c92);
// [0] diffuse, [1] specular, [2] ambient (w: the reflection's weight facing
// the viewer), [5] x reflection weight, z specular exponent, w alpha.
float4 materialData[6] : register(c93);

struct Input {
    float4 uvDepthFade : TEXCOORD0;  // uv, view depth, shadow fade
    float3 tangent : TEXCOORD1;
    float3 binormal : TEXCOORD2;
    float3 normal : TEXCOORD3;
    float3 extinction : TEXCOORD4;   // atmosphere: colour * this + inscatter
    float3 inscatter : TEXCOORD5;
    float3 worldPos : TEXCOORD6;
#if MC2VR_PGDIFF_AMBOCC
    float4 ambOcc : COLOR0;          // baked ambient occlusion (rgb, alpha)
#endif
};

// The normal map holds the tangent-space normal's x in alpha and y in green
// (z reconstructed), applied in the interpolated tangent frame. The
// originals' z is sqrt(1 - x² - 2y), y unsquared: a slip in the game's
// source (meant: 1 - x² - y²), kept so the shading stays the originals'.
float3 mapped_normal(Input i)
{
    const float2 xy = 2 * (tex2D(normalMap, i.uvDepthFade.xy).wy - 0.5);
    const float z = sqrt(1 - xy.x * xy.x - 2 * xy.y);
    return normalize(xy.x * i.tangent + xy.y * i.binormal + z * i.normal);
}

float4 main(Input i) : COLOR
{
    const float2 uv = i.uvDepthFade.xy;
    const float3 normal = mapped_normal(i);
    const float3 view = normalize(cameraPos - i.worldPos);

    // The sun: diffuse by its visibility; the specular highlight is kept at a
    // quarter of it in shadow.
    const float visibility = sun_visibility(i.worldPos, i.uvDepthFade.z, i.uvDepthFade.w);
    const float3 toSun = -globalLightData[0].xyz;
    const float3 sunDiffuse = visibility * globalLightData[1].rgb * max(dot(normal, toSun), 0);
    const float3 sunSpecular = max(0.25, visibility) * globalLightData[1].rgb *
                               blinn_phong(normal, toSun, view, materialData[5].z);

    const LocalLights local = local_lights_spec(i.worldPos, normal, view, materialData[5].z);
    const float3 diffuse = saturate(sunDiffuse + local.diffuse);
    const float3 light = diffuse * materialData[0].rgb + ambient_cube(normal) * materialData[2].rgb;

    const float4 albedo = tex2D(diffuseMap, uv);
    const float3 specMap = tex2D(specularMap, uv).rgb;
    const float3 specular = (sunSpecular + local.specular) * specMap * materialData[1].rgb;
    float3 colour = albedo.rgb * light + specular;

#if MC2VR_PGDIFF_METAL
    // Reflection: Fresnel (1 - N.V)^5 blends materialData[2].w (facing) to 1
    // (grazing), tinted by the specular map.
    const float fresnel = pow(1 - max(dot(view, normal), 0), 5);
    const float weight = lerp(materialData[2].w, 1, fresnel) * materialData[5].x;
    colour += texCUBE(reflectionMap, reflect(-view, normal)).rgb * specMap * weight;
#endif

    float alpha = albedo.a * materialData[5].w;
#if MC2VR_PGDIFF_AMBOCC
    colour *= i.ambOcc.rgb;
    alpha *= i.ambOcc.a;
#endif

    colour = colour * i.extinction + i.inscatter;
    // The highlight's luminance shows through transparent parts.
    const float specLuminance = dot(specular, float3(0.299, 0.587, 0.114));
    return float4(min(max(colour, 1e-5), 30000), saturate(max(alpha, specLuminance)));
}
