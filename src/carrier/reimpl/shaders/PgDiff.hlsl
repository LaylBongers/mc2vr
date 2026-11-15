// The PgDiff* material families (the game's high-quality "PgDiff<features>FP"
// shaders; docs/shaders.md) — reimplementation. One source, built per family
// with feature defines and, like the other families, in 7 light variants
// (lights.hlsli's defines). The family name lists its features; e.g.
//
//   PgDiffFP                (shader3.bin@0x42b00 ..)  (none)
//   PgDiffSpecNormAmbOccFP  (shader3.bin@0xd9c70 ..)  SPEC NORM AMBOCC
//   PgDiffSpecMetalNormFP   (shader3.bin@0xb5270 ..)  SPEC NORM METAL
//
//   MC2VR_PGDIFF_SPEC    specularMap: Blinn-Phong specular from the sun and the
//                        local lights, its luminance raising the alpha
//   MC2VR_PGDIFF_NORM    normalMap: a two-channel tangent-space normal, and
//                        the ambient cube; without it, the interpolated
//                        normal and a per-vertex ambient (COLOR1)
//   MC2VR_PGDIFF_AMBOCC  the vertex colour (COLOR0) multiplies the lit colour
//                        and the alpha
//   MC2VR_PGDIFF_METAL   reflectionMap: a cube reflection, tinted by the
//                        specular map, weighted by a Fresnel term
//   MC2VR_PGDIFF_REFL    reflectionMap: the same reflection at a constant
//                        weight (no Fresnel)
//   MC2VR_PGDIFF_EMIS    emissiveMap: an unlit colour added to the lit one
//   MC2VR_PGDIFF_RIM     a rim light: the global rim colour towards the
//                        silhouette, away from the sun
//   MC2VR_PGDIFF_SSS     subsurface scattering: a coloured band around the
//                        sun's terminator (with Rim, also the rim light's)
//
// The game builds Spec with any of Norm, Metal, Refl and SSS, and Emis
// without Spec; Rim and AmbOcc go with all of them. Every feature's place in
// the lit colour is the originals' (the combined families settle it: the
// reflection is added before the occlusion multiplies, e.g.
// PgDiffSpecMetalNormAmbOccFP, shader3.bin@0x9c230).
//
// MC2VR_SHADING_MODERN 0 | 1 (conf shading=classic|modern): 0 is the
// originals' shading (below, `main` under !MC2VR_SHADING_MODERN); 1 a
// physically based model in the originals' interface (the `_modern` symbols;
// the model, the material derivation and the constants at the modern main).
//
// Unlike our other receivers' originals, these already take the game's 9-tap
// sun-shadow filter: shadow.hlsli is theirs, so the math is the originals'
// throughout. Owning the lookup is the point (a cascade blend band, docs/shadows.md).

#ifndef MC2VR_SHADING_MODERN
#define MC2VR_SHADING_MODERN 0
#endif
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
#ifndef MC2VR_PGDIFF_REFL
#define MC2VR_PGDIFF_REFL 0
#endif
#ifndef MC2VR_PGDIFF_EMIS
#define MC2VR_PGDIFF_EMIS 0
#endif
#ifndef MC2VR_PGDIFF_RIM
#define MC2VR_PGDIFF_RIM 0
#endif
#ifndef MC2VR_PGDIFF_SSS
#define MC2VR_PGDIFF_SSS 0
#endif

#if (MC2VR_PGDIFF_NORM || MC2VR_PGDIFF_METAL || MC2VR_PGDIFF_REFL || MC2VR_PGDIFF_SSS) && !MC2VR_PGDIFF_SPEC
#error "PgDiff.hlsl: Norm, Metal, Refl and SSS come with Spec (no such family in the game)"
#endif
#if MC2VR_PGDIFF_EMIS && MC2VR_PGDIFF_SPEC
#error "PgDiff.hlsl: Emis and Spec share sampler s1 (no such family in the game)"
#endif
#if MC2VR_PGDIFF_METAL && MC2VR_PGDIFF_REFL
#error "PgDiff.hlsl: Metal and Refl are two forms of one reflection"
#endif

#include "shadow.hlsli"
#include "lights.hlsli"

sampler2D diffuseMap : register(s0);
#if MC2VR_PGDIFF_SPEC
sampler2D specularMap : register(s1);
#endif
#if MC2VR_PGDIFF_EMIS
sampler2D emissiveMap : register(s1);
#endif
#if MC2VR_PGDIFF_NORM
sampler2D normalMap : register(s2);
#endif
#if MC2VR_PGDIFF_METAL || MC2VR_PGDIFF_REFL
samplerCUBE reflectionMap : register(s13);
#endif

float3 cameraPos : register(c92);
// [0] diffuse, [1] specular, [2] ambient (w: the metal reflection's weight
// facing the viewer), [3] emissive, [4] subsurface colour (w: its wrap),
// [5] x reflection weight, z specular exponent, w alpha.
float4 materialData[6] : register(c93);

struct Input {
    float4 uvDepthFade : TEXCOORD0;  // uv, view depth, shadow fade
#if MC2VR_PGDIFF_NORM
    float3 tangent : TEXCOORD1;
    float3 binormal : TEXCOORD2;
#endif
    float3 normal : TEXCOORD3;
    float3 extinction : TEXCOORD4;   // atmosphere: colour * this + inscatter
    float3 inscatter : TEXCOORD5;
    float3 worldPos : TEXCOORD6;
#if MC2VR_PGDIFF_AMBOCC
    float4 ambOcc : COLOR0;          // baked ambient occlusion (rgb, alpha)
#endif
#if !MC2VR_PGDIFF_NORM
    float3 ambient : COLOR1;         // the vertex's ambient light
#endif
};

#if MC2VR_PGDIFF_NORM
// The normal map holds the tangent-space normal's x in alpha and y in green
// (z reconstructed), applied in the interpolated tangent frame. The
// originals' z is sqrt(1 - x² - 2y), y unsquared: a slip in the game's
// source (meant: 1 - x² - y²), kept so the shading stays the originals'.
// The modern shading reconstructs it properly.
float3 mapped_normal(Input i)
{
    const float2 xy = 2 * (tex2D(normalMap, i.uvDepthFade.xy).wy - 0.5);
#if MC2VR_SHADING_MODERN
    const float z = sqrt(saturate(1 - xy.x * xy.x - xy.y * xy.y));
#else
    const float z = sqrt(1 - xy.x * xy.x - 2 * xy.y);
#endif
    return normalize(xy.x * i.tangent + xy.y * i.binormal + z * i.normal);
}
#endif

float3 surface_normal(Input i)
{
#if MC2VR_PGDIFF_NORM
    return mapped_normal(i);
#else
    return normalize(i.normal);
#endif
}

// The ambient light: the ambient cube at the mapped normal, or the vertex's.
float3 ambient_light(Input i, float3 normal)
{
#if MC2VR_PGDIFF_NORM
    return ambient_cube(normal);
#else
    return i.ambient;
#endif
}

#if MC2VR_PGDIFF_SSS
// Subsurface scattering (materialData[4]: colour, w wrap): a wrapped diffuse
// (the lighting term remapped from [-wrap, 1]) minus the plain one, both
// smoothstepped. Of N.L, a band around the sun's terminator; unclamped (the
// colour takes max(band, 0)).
float subsurface_band(float x)
{
    const float wrap = materialData[4].w;
    return smoothstep(-wrap, 1, x) - smoothstep(0, 1, x);
}
#endif

#if !MC2VR_SHADING_MODERN
float4 main(Input i) : COLOR
{
    const float2 uv = i.uvDepthFade.xy;
    const float3 normal = surface_normal(i);
    const float3 view = normalize(cameraPos - i.worldPos);

    // The sun: diffuse by its visibility (with Spec, the highlight is kept at
    // a quarter of it in shadow).
    const float visibility = sun_visibility(i.worldPos, i.uvDepthFade.z, i.uvDepthFade.w);
    const float3 toSun = -globalLightData[0].xyz;
    const float ndl = dot(normal, toSun);
    float3 direct = visibility * globalLightData[1].rgb * max(ndl, 0);
#if MC2VR_PGDIFF_SSS
    // Subsurface scattering of the sun, unshadowed.
    const float sunBand = subsurface_band(ndl);
    direct += max(sunBand, 0) * materialData[4].rgb;
#endif
#if MC2VR_PGDIFF_RIM
    // The rim light (globalLightData[2]: colour, w bias): stronger where the
    // surface turns from the viewer, and away from the sun (1 - N.L, N.L
    // unclamped).
    const float rimTerm = dot(-view, normal) + globalLightData[2].w;
    direct += max(rimTerm, 0) * globalLightData[2].rgb * (1 - ndl);
#if MC2VR_PGDIFF_SSS
    // The rim scatters too: its term through the same band, where the sun's
    // band leaves room (1 - band, unclamped).
    direct += max(subsurface_band(rimTerm), 0) * materialData[4].rgb * (1 - sunBand);
#endif
#endif
#if MC2VR_PGDIFF_SPEC
    const float3 sunSpecular = max(0.25, visibility) * globalLightData[1].rgb *
                               blinn_phong(normal, toSun, view, materialData[5].z);
    const LocalLights local = local_lights_spec(i.worldPos, normal, view, materialData[5].z);
    direct += local.diffuse;
#else
    direct += local_lights(i.worldPos, normal);
#endif
    const float3 light = saturate(direct) * materialData[0].rgb + ambient_light(i, normal) * materialData[2].rgb;

    const float4 albedo = tex2D(diffuseMap, uv);
    float3 colour = albedo.rgb * light;

#if MC2VR_PGDIFF_SPEC
    const float3 specMap = tex2D(specularMap, uv).rgb;
    const float3 specular = (sunSpecular + local.specular) * specMap * materialData[1].rgb;
    colour += specular;
#endif

#if MC2VR_PGDIFF_METAL
    // Reflection: Fresnel (1 - N.V)^5 blends materialData[2].w (facing) to 1
    // (grazing), tinted by the specular map.
    const float fresnel = pow(1 - max(dot(view, normal), 0), 5);
    const float weight = lerp(materialData[2].w, 1, fresnel) * materialData[5].x;
    colour += texCUBE(reflectionMap, reflect(-view, normal)).rgb * specMap * weight;
#endif
#if MC2VR_PGDIFF_REFL
    // Reflection at a constant weight, tinted by the specular map.
    colour += texCUBE(reflectionMap, reflect(-view, normal)).rgb * specMap * materialData[5].x;
#endif

#if MC2VR_PGDIFF_EMIS
    colour += tex2D(emissiveMap, uv).rgb * materialData[3].rgb;
#endif

    float alpha = albedo.a * materialData[5].w;
#if MC2VR_PGDIFF_AMBOCC
    colour *= i.ambOcc.rgb;
    alpha *= i.ambOcc.a;
#endif

    colour = colour * i.extinction + i.inscatter;
#if MC2VR_PGDIFF_SPEC
    // The highlight's luminance shows through transparent parts.
    alpha = saturate(max(alpha, dot(specular, float3(0.299, 0.587, 0.114))));
#endif
    return float4(min(max(colour, 1e-5), 30000), alpha);
}
#endif  // !MC2VR_SHADING_MODERN

#if MC2VR_SHADING_MODERN
// ---- The modern shading: physically based, in the originals' interface -------------
//
// Direct light (the sun and every local light): GGX specular and Burley
// diffuse with the energy split (lights.hlsli, add_pbr_light), the sun fully
// shadowed. Ambient: the family's ambient (the cube at the normal, or the
// vertex's) for the diffuse; for the specular, the ambient along the
// reflection vector (the cube at R, or the vertex's), weighted by the
// split-sum fit (env_brdf_approx). The reflective families (Metal, Refl) get
// their reflection cube as a coat over the full-colour base instead, weighted
// by the artists' own controls (below). The rim, subsurface and emissive
// terms are the originals', added on top.
//
// The material from the Blinn-Phong assets:
//   GGX alpha  = sqrt(2 / (n + 2)), n = materialData[5].z (the Blinn-Phong
//                exponent; the Beckmann-equivalent width), clamped to
//                [kAlphaMin, 1]; without Spec, kNoSpecAlpha
//   tint       = saturate(specularMap * materialData[1]) (the specular colour,
//                as the originals weight their highlight)
//   dielectric F0 = kDielectricF0 + tint * kSpecF0Scale (without Spec,
//                kDielectricF0)
//   Every surface is a dielectric with its full albedo. The game's "Metal" is
//   an environment-mapped reflective material (car paint, glass, polished
//   panels), not bare metal: treating bright specular-map texels as metal
//   (no diffuse, reflection tinted by the grey map) washed coloured paint out
//   to grey (live, 2026-11-15).
//   Metal: the artists' reflection, as in the originals: the reflection
//                cube times the specular map, weighted by the original's
//                Fresnel lerp(materialData[2].w, 1, (1 - N.V)^5) *
//                materialData[5].x and added over the full-colour base. Every
//                reflective material in the game is authored this way (a
//                material census, 2026-11-15: Fresnel bases 0..0.29 only,
//                no metals), so the look the art was tuned for is kept; only
//                the lighting underneath is modern.
//   Refl:  the same with the original's constant weight materialData[5].x.
//   The cube is read near-sharp, as the originals do: the mip follows GGX
//   alpha (not its square root), capped at kReflectionMipMax. A blur by
//   sqrt(alpha) * 6 (~mip 3 on glossy paint) made the reflections look weak
//   (live, 2026-11-15).
//
// The view is the camera's: the families with neither Spec nor Rim (PgDiffFP,
// AmbOcc, Emis, EmisAmbOcc) have no cameraPos in their originals' interface;
// their modern builds declare it at the same register, and the game uploads
// it there by name (kAddsCameraPos in hooks/shader_reimpl.cpp), per eye.
#include "modern.hlsli"

static const float kSpecF0Scale = 0.25;   // specular map -> extra dielectric F0 (calibration)
static const float kAlphaMin = 0.03;      // the sharpest highlight (GGX alpha)
static const float kNoSpecAlpha = 0.6;    // GGX alpha of the families without a specular map
static const float kEnvStrength = 1.0;    // environment specular scale (calibration)
static const float kReflectionMips = 6;   // reflectionMap mip per unit of GGX alpha
static const float kReflectionMipMax = 1; // the blurriest reflection mip (the originals read level 0)

float4 main(Input i) : COLOR
{
    const float2 uv = i.uvDepthFade.xy;
    const float3 normal = surface_normal(i);
    const float3 view = normalize(cameraPos - i.worldPos);
    const float4 albedo = tex2D(diffuseMap, uv);
#if MC2VR_PGDIFF_AMBOCC
    const float3 ao = i.ambOcc.rgb;
#else
    const float3 ao = 1;
#endif

    // ---- the material ----
    PbrMaterial m;
#if MC2VR_PGDIFF_SPEC
    const float3 specMap = tex2D(specularMap, uv).rgb;
    const float3 tint = saturate(specMap * materialData[1].rgb);
    m.alpha = clamp(sqrt(2 / (max(materialData[5].z, 0) + 2)), kAlphaMin, 1);
    m.f0 = kDielectricF0 + tint * kSpecF0Scale;
#else
    m.alpha = kNoSpecAlpha;
    m.f0 = kDielectricF0;
#endif
    m.roughness = sqrt(m.alpha);
    m.ndv = abs(dot(normal, view)) + 1e-5;
    const float3 diffuseAlbedo = albedo.rgb;

    // ---- direct light ----
    const float visibility = sun_visibility(i.worldPos, i.uvDepthFade.z, i.uvDepthFade.w);
    const float3 toSun = -globalLightData[0].xyz;
    LocalLights direct = direct_light_pbr(i.worldPos, normal, view, m, visibility);

    // The originals' extra terms (their place in the saturated direct light).
    float3 extra = 0;
    const float ndl = dot(normal, toSun);
#if MC2VR_PGDIFF_SSS
    const float sunBand = subsurface_band(ndl);
    extra += max(sunBand, 0) * materialData[4].rgb;
#endif
#if MC2VR_PGDIFF_RIM
    const float rimTerm = dot(-view, normal) + globalLightData[2].w;
    extra += max(rimTerm, 0) * globalLightData[2].rgb * (1 - ndl);
#if MC2VR_PGDIFF_SSS
    extra += max(subsurface_band(rimTerm), 0) * materialData[4].rgb * (1 - sunBand);
#endif
#endif

    // ---- ambient and environment ----
    const float3 envBrdf = env_brdf_approx(m.f0, m.roughness, m.ndv);
    const float3 reflected = reflect(-view, normal);
    const float3 ambient = ambient_light(i, normal) * materialData[2].rgb;
#if MC2VR_PGDIFF_METAL || MC2VR_PGDIFF_REFL
    // The reflective coat, weighted as the artists set it.
#if MC2VR_PGDIFF_METAL
    const float coat = lerp(materialData[2].w, 1, pow5(1 - min(m.ndv, 1))) * materialData[5].x;
#else
    const float coat = materialData[5].x;
#endif
    const float mip = min(m.alpha * kReflectionMips, kReflectionMipMax);
    const float3 reflection = texCUBElod(reflectionMap, float4(reflected, mip)).rgb;
    const float3 specular = direct.specular + reflection * specMap * (coat * ao);
#else
    const float3 environment = ambient_light(i, reflected) * materialData[2].rgb;
    const float3 specular = direct.specular + environment * envBrdf * (kEnvStrength * ao);
#endif
    float3 colour = diffuseAlbedo * (direct.diffuse * materialData[0].rgb + ambient * (1 - envBrdf) * ao) +
                    albedo.rgb * saturate(extra) * materialData[0].rgb * ao + specular;
#if MC2VR_PGDIFF_EMIS
    colour += tex2D(emissiveMap, uv).rgb * materialData[3].rgb * ao;
#endif

    float alpha = albedo.a * materialData[5].w;
#if MC2VR_PGDIFF_AMBOCC
    alpha *= i.ambOcc.a;
#endif

    colour = colour * i.extinction + i.inscatter;
#if MC2VR_PGDIFF_SPEC
    // The highlight's luminance shows through transparent parts.
    alpha = saturate(max(alpha, dot(specular, kLuminance)));
#endif
    return float4(min(max(colour, 1e-5), 30000), alpha);
}
#endif  // MC2VR_SHADING_MODERN
