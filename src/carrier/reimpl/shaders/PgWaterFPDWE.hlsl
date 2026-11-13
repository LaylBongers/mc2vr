// PgWaterFPDWE (shader3.bin@0x254a80; docs/shaders.md) — reimplementation.
// A water surface variant: refraction through a depth-coloured body of water,
// a reflection, and the wake mask. The original takes a single binary shadow
// tap; this one uses the game's own 9-tap filter (shadow.hlsli). Everything
// else, including the interface, is the original's (which was compiled with
// little optimization: the redundant moves are not reproduced).

#include "shadow.hlsli"

float4 viewport : register(c0);       // zw = 1 / render size
float4 ProjectParams : register(c1);  // depth linearization: y / (z - x); z = the depth colour's range
float3 cameraPos : register(c92);
float4 materialData[6] : register(c93);  // [0] diffuse, [2] ambient (w = Fresnel base), [5].x reflection strength
sampler2D ReflectionMap : register(s0);
sampler2D DepthMap : register(s2);
sampler2D DepthColorMap : register(s3);
sampler2D RefractionMap : register(s4);
sampler2D WakeNormalMap : register(s5);  // 128x128; the wake mask in alpha

struct Input {
    float3 normal0 : NORMAL0;        // the surface normal is cross(normal1, normal0), unnormalized
    float3 normal1 : NORMAL1;
    float3 worldPos : TEXCOORD2;
    float4 refraction : TEXCOORD3;   // projected refraction coordinates (z also scales the offset)
    float4 extinction : TEXCOORD4;   // atmosphere: colour * this + inscatter; w = wake u
    float4 inscatter : TEXCOORD5;    // w = wake v
    float2 depthFade : TEXCOORD6;    // view depth, shadow fade
    float4 ambient : COLOR0;         // w = the surface's projected depth
    float2 vpos : VPOS;
};

float linear_depth(float z)
{
    return (1 / (z - ProjectParams.x)) * ProjectParams.y;
}

// The wake mask's alpha, filtered bilinearly by hand over its 128² texels.
float wake_alpha(float2 uv)
{
    const float2 t = uv * 128;
    const float2 f = t - floor(t);
    const float a = tex2D(WakeNormalMap, uv).w;
    const float b = tex2D(WakeNormalMap, uv + float2(1.0 / 128, 0)).w;
    const float c = tex2D(WakeNormalMap, uv + float2(0, 1.0 / 128)).w;
    const float d = tex2D(WakeNormalMap, uv + 1.0 / 128).w;
    const float top = a + f.x * (b - a);
    return top + f.y * ((c + f.x * (d - c)) - top);
}

float clamp01(float x)
{
    x = (x - 0 >= 0) ? x : 0;
    return (x - 1 >= 0) ? 1 : x;
}

float4 main(Input i) : COLOR
{
    const float mask = (0.5 - wake_alpha(float2(i.extinction.w, i.inscatter.w)) >= 0) ? 0 : 1;
    clip((-(mask * 2) >= 0) ? -1 : 0);

    const float3 n = cross(i.normal1, i.normal0);
    const float2 refractUv = (i.refraction.xy - n.xz * i.refraction.z) * (1 / i.refraction.w);
    const float2 screenUv = (i.vpos + 0.5) * viewport.zw;

    // Water thickness behind the surface: at the refracted point, or, where
    // that lies in front of the water, straight behind.
    const float sceneDepth = linear_depth(tex2D(DepthMap, screenUv).x);
    const float refractDepth = linear_depth(tex2D(DepthMap, refractUv).x);
    const float surfaceDepth = linear_depth(i.ambient.w);
    const float refractInFront = (surfaceDepth - refractDepth >= 0) ? 1 : 0;
    const float shoreDepth = sceneDepth - surfaceDepth;
    const float thickness = (-refractInFront >= 0) ? refractDepth - surfaceDepth : shoreDepth;
    const float4 depthColour = tex2D(DepthColorMap, clamp01(thickness * (1 / ProjectParams.z)).xx);

    const float3 toEye = cameraPos - i.worldPos;
    const float3 r = reflect(-(toEye * rsqrt(dot(toEye, toEye))), n);

    const float vis = lerp(1, shadow_9tap(shadow_coords(i.worldPos, i.depthFade.x)), clamp01(i.depthFade.y));
    const float ndl = max(dot(n, -globalLightData[0].xyz), 0);
    const float3 light = (vis * globalLightData[1].rgb * ndl) * materialData[0].rgb + materialData[2].rgb * i.ambient.rgb;
    const float3 litDepth = depthColour.rgb * light;

    const float f = 1 - max(dot(n, r), 0);
    const float fresnel = (1 - materialData[2].w) * (f * ((f * f) * (f * f))) + materialData[2].w;

    const float3 screenRefraction = tex2D(RefractionMap, screenUv).rgb;
    const float3 refracted = (-refractInFront >= 0) ? tex2D(RefractionMap, refractUv).rgb : screenRefraction;
    const float3 body = (-(depthColour.w - 1 >= 0 ? 0 : 1) >= 0) ? litDepth : lerp(refracted, litDepth, depthColour.w);
    const float3 surface = lerp(body, tex2D(ReflectionMap, refractUv).rgb, fresnel * materialData[5].x);

    float3 colour = surface * i.extinction.rgb + i.inscatter.rgb;
    colour = (colour - 1e-5 >= 0) ? colour : 1e-5;
    colour = (colour - 30000 >= 0) ? 30000 : colour;
    colour = lerp(colour, screenRefraction, clamp01(1 - shoreDepth * 0.95));
    return float4((-mask >= 0) ? 0 : colour, mask);
}
