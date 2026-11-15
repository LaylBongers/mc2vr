// The modern shading's shared parts (conf shading=modern; docs/shaders.md
// "Modern shading"): not in the game. The per-light BRDF is lights.hlsli's
// (add_pbr_light, local_lights_pbr, env_brdf_approx); this is what every
// modern family builds on it: the sun with the local lights, the view-side
// setup of a plain dielectric, and the ambient/environment split.
//
// The view direction needs the camera position (c92). Each family declares
// it; the game uploads it per eye wherever a shader declares one, also for
// the families whose original has none: it resolves constants by name from
// the shader object we hand it (the registry's kAddsCameraPos entries,
// hooks/shader_reimpl.cpp).

#ifndef MC2VR_MODERN_HLSLI
#define MC2VR_MODERN_HLSLI

#include "global_light.hlsli"
#include "lights.hlsli"

static const float kDielectricF0 = 0.04;  // the common dielectric's 4%
static const float3 kLuminance = float3(0.299, 0.587, 0.114);

// The direct light through the BRDF: the variant's local lights and the sun
// (globalLightData[0..1]) at its shadow visibility.
LocalLights direct_light_pbr(float3 worldPos, float3 normal, float3 view, PbrMaterial m, float visibility)
{
    LocalLights direct = local_lights_pbr(worldPos, normal, view, m);
    add_pbr_light(direct, visibility * globalLightData[1].rgb, -globalLightData[0].xyz, normal, view, m);
    return direct;
}

// A plain dielectric (F0 = kDielectricF0) of GGX alpha `alpha`, seen along
// `view` (unit, towards the camera).
PbrMaterial dielectric(float alpha, float3 normal, float3 view)
{
    PbrMaterial m;
    m.alpha = alpha;
    m.f0 = kDielectricF0;
    m.roughness = sqrt(alpha);
    m.ndv = abs(dot(normal, view)) + 1e-5;
    return m;
}

// A plain dielectric surface lit: the albedo times the direct diffuse (scaled
// by diffuseScale, the material's diffuse colour) and the ambient through the
// energy split (1 - EnvBRDF), plus the direct specular and the environment
// (the ambient along the reflection vector) weighted by the split-sum fit
// times envStrength. ambient and environment carry the material's ambient
// colour already.
float3 dielectric_colour(float3 albedo, PbrMaterial m, LocalLights direct, float3 diffuseScale, float3 ambient,
                         float3 environment, float envStrength)
{
    const float3 envBrdf = env_brdf_approx(m.f0, m.roughness, m.ndv);
    return albedo * (direct.diffuse * diffuseScale + ambient * (1 - envBrdf)) + direct.specular +
           environment * envBrdf * envStrength;
}

#endif
