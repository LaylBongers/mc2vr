// PgWaterFP (shader3.bin@0x2571d0), PgWaterFPbl (@0x24fe80) and PgWaterFP_LI
// (@0x25c3d0) (docs/shaders.md) — reimplementation, one source for the three.
// The open water surface: wake normals, two sets of four Gerstner waves added
// per pixel, refraction through a depth-coloured body of water, a reflection,
// sun diffuse and specular, and foam. The originals take a single binary
// shadow tap; this one uses the game's own 9-tap filter (shadow.hlsli).
// Everything else, including the interface, is the originals' (which were
// compiled with little optimization: the redundant moves are not reproduced).
//
// Variants (src/carrier/CMakeLists.txt):
//   PgWaterFP       the wake maps filtered bilinearly by hand;
//   MC2VR_WATER_BL  (PgWaterFPbl) the wake maps sampled once (hardware filter);
//   MC2VR_WATER_LI  (PgWaterFP_LI) the wake normal map sampled once, no foam
//                   mask map (foam from the wake normal alone, scaled 128
//                   where the others scale it 8), no shoreline
//                   fade and an opaque, unmasked output.

#include "shadow.hlsli"

#ifndef MC2VR_WATER_BL
#define MC2VR_WATER_BL 0
#endif
#ifndef MC2VR_WATER_LI
#define MC2VR_WATER_LI 0
#endif

float4 viewport : register(c0);       // zw = 1 / render size
float4 ProjectParams : register(c1);  // depth linearization: y / (z - x); z = the depth colour's range
float3 cameraPos : register(c92);
float4 materialData[6] : register(c93);  // [0] diffuse, [1] specular, [2] ambient (w = Fresnel base),
                                         // [5].x reflection strength, .z specular power
// Two sets of four Gerstner waves, [0..5] and [6..11], one wave per component:
// direction x, direction z, steepness, footprint scale, amplitude, phase.
float4 Waves[12] : register(c132);
sampler2D ReflectionMap : register(s0);
sampler2D FoamMap : register(s1);
sampler2D DepthMap : register(s2);
sampler2D DepthColorMap : register(s3);
sampler2D RefractionMap : register(s4);
sampler2D WakeNormalMap : register(s5);    // 128x128: xyz the wake normal (encoded), w the wake mask
#if !MC2VR_WATER_LI
sampler2D WakeFoamMaskMap : register(s6);  // 128x128: x foam where >= 0, y foam (over the wake's y)
#endif

struct Input {
    float3 normal0 : NORMAL0;        // the surface normal is cross(normal1, normal0), unnormalized
    float4 normal1 : NORMAL1;        // w = the vertex's footprint (the waves' fade)
    float3 worldPos : TEXCOORD2;
    float4 refraction : TEXCOORD3;   // projected refraction coordinates (z also scales the offset)
    float4 extinction : TEXCOORD4;   // atmosphere: colour * this + inscatter; w = wake u
    float4 inscatter : TEXCOORD5;    // w = wake v
    float2 depthFade : TEXCOORD6;    // view depth, shadow fade
    float4 ambient : COLOR0;         // w = the surface's projected depth
    float2 vpos : VPOS;
};

float clamp01(float x)
{
    x = (x - 0 >= 0) ? x : 0;
    return (x - 1 >= 0) ? 1 : x;
}

float4 clamp01(float4 x)
{
    x = (x - 0 >= 0) ? x : 0;
    return (x - 1 >= 0) ? 1 : x;
}

float linear_depth(float z)
{
    return (1 / (z - ProjectParams.x)) * ProjectParams.y;
}

// A 128² wake map at uv: filtered bilinearly by hand (PgWaterFP) or by the
// sampler (the others).
float4 wake_sample(sampler2D map, float2 uv)
{
#if MC2VR_WATER_BL || MC2VR_WATER_LI
    return tex2D(map, uv);
#else
    const float2 t = uv * 128;
    const float2 f = t - floor(t);
    const float4 a = tex2D(map, uv);
    const float4 b = tex2D(map, uv + float2(1.0 / 128, 0));
    const float4 c = tex2D(map, uv + float2(0, 1.0 / 128));
    const float4 d = tex2D(map, uv + 1.0 / 128);
    const float4 top = a + f.x * (b - a);
    return top + f.y * ((c + f.x * (d - c)) - top);
#endif
}

// The wake normal from the map's xyz: x and z are stored offset by y and
// scaled by min(y, 1); y = 0 means no wake (straight up).
float3 wake_normal(float3 w)
{
    const float scale = (w.y - 1 >= 0) ? 1 : w.y;
    const float2 xz = (w.xz - w.y) * scale;
    const float3 n = float3(xz.x, w.y - xz.x * xz.x - xz.y * xz.y, xz.y);
    return (w.y * w.y > 0) ? normalize(n) : float3(0, 1, 0);
}

// One set of four Gerstner waves (Waves[k..k+5]) added to the surface's
// tangents. Each wave fades in where the vertex's footprint is too coarse to
// carry it and out where the pixel's footprint is too coarse to show it.
void add_waves(inout float3 n0, inout float3 n1, const int k, float2 xz, float pixelFootprint,
               float vertexFootprint)
{
    const float4 fade = clamp01(clamp01(2 - pixelFootprint * Waves[k + 3]) -
                                clamp01(2 - Waves[k + 3] * vertexFootprint));
    [branch] if (dot(fade, fade) > 0) {
        const float4 dirX = Waves[k];
        const float4 dirZ = Waves[k + 1];
        const float4 phase = dirX * xz.x + dirZ * xz.y - Waves[k + 5];
        const float4 amplitude = fade * Waves[k + 4];
        const float4 q = -(Waves[k + 2] * (sin(phase) * amplitude));
        const float4 c = cos(phase) * amplitude;
        n0 += float3(dot(q * dirX, dirX), dot(c, dirX), dot(q * dirZ, dirX));
        n1 += float3(dot(q * dirX, dirZ), dot(c, dirZ), dot(q * dirZ, dirZ));
    }
}

float4 main(Input i) : COLOR
{
    // The wake mask: only where its alpha exceeds 1/2.
    const float2 wakeUv = float2(i.extinction.w, i.inscatter.w);
    const float4 wake = wake_sample(WakeNormalMap, wakeUv);
    const float mask = (0.5 - wake.w >= 0) ? 0 : 1;
    clip((-(mask * 2) >= 0) ? -1 : 0);
    const float3 wakeN = wake_normal(wake.xyz);

    // Foam from the wake: where its normal tilts (and, with the mask map, where
    // that says so).
#if MC2VR_WATER_LI
    float foam = clamp01((1 - wakeN.y) * 128);  // (the others: 8)
#else
    const float2 foamMask = wake_sample(WakeFoamMaskMap, wakeUv).xy;
    float foam = clamp01((1 - wakeN.y) * 8 * step(0, foamMask.x) + foamMask.y / wake.y);
#endif

    // The waves' normal, cross(normal1, normal0), taken in the wake normal's
    // frame.
    const float pixelFootprint = length(ddx(i.worldPos.xz) + ddy(i.worldPos.xz));
    float3 n0 = i.normal0;
    float3 n1 = i.normal1.xyz;
    add_waves(n0, n1, 0, i.worldPos.xz, pixelFootprint, i.normal1.w);
    add_waves(n0, n1, 6, i.worldPos.xz, pixelFootprint, i.normal1.w);
    const float3 w = cross(n1, n0);
    const float3 t = cross(float3(1, 0, 0), wakeN);
    const float3 b = cross(wakeN, t);
    const float3 n = normalize(b * w.x + wakeN * w.y + t * w.z);

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
    const float3 v = toEye * rsqrt(dot(toEye, toEye));
    const float3 r = reflect(-v, n);

    // The sun: diffuse, and a specular that keeps a quarter in shadow.
    const float vis = lerp(1, shadow_9tap(shadow_coords(i.worldPos, i.depthFade.x)), clamp01(i.depthFade.y));
    const float3 sun = vis * globalLightData[1].rgb;
    const float3 specularLight = max(vis, 0.25) * globalLightData[1].rgb;
    const float3 l = -globalLightData[0].xyz;
    const float ndl = dot(n, l);
    const float ndh = dot(normalize(v + l), n);
    const float lit = (ndl > 0) ? 1 : 0;
    const float specular = pow(abs(ndh), materialData[5].z) * (lit * ((ndh > 0) ? 1 : 0));
    const float3 sunDiffuse = sun * (ndl * lit);
    const float3 litDepth = depthColour.rgb * (sunDiffuse * materialData[0].rgb + materialData[2].rgb * i.ambient.rgb) +
                            specularLight * specular * materialData[1].rgb;

    const float f = 1 - max(dot(n, r), 0);
    const float fresnel = (1 - materialData[2].w) * (f * ((f * f) * (f * f))) + materialData[2].w;

    const float3 screenRefraction = tex2D(RefractionMap, screenUv).rgb;
    const float3 refracted = (-refractInFront >= 0) ? tex2D(RefractionMap, refractUv).rgb : screenRefraction;
    const float3 body = (-(depthColour.w - 1 >= 0 ? 0 : 1) >= 0) ? litDepth : lerp(refracted, litDepth, depthColour.w);
    float3 surface = lerp(body, tex2D(ReflectionMap, refractUv).rgb, fresnel * materialData[5].x);

    // Foam, more of it in the shallows: two scrolling lookups, lit by the sun
    // and the ambient.
    foam += clamp01(1 - shoreDepth * 0.33);
    const float4 foamTex = (tex2D(FoamMap, (refractUv + i.worldPos.xz) * 0.1) +
                            tex2D(FoamMap, (i.worldPos.xz - refractUv + 0.5) * 0.15)) * 0.5;
    const float3 foamColour = foamTex.rgb * (sunDiffuse + i.ambient.rgb);
    surface = (foam * foam > 0) ? lerp(surface, foamColour, foam * foamTex.w * 0.85) : surface;

    float3 colour = surface * i.extinction.rgb + i.inscatter.rgb;
    colour = (colour - 1e-5 >= 0) ? colour : 1e-5;
    colour = (colour - 30000 >= 0) ? 30000 : colour;
#if MC2VR_WATER_LI
    return float4(colour, 1);
#else
    // Fade to the plain scene at the shoreline.
    colour = lerp(colour, screenRefraction, clamp01(1 - shoreDepth * 0.95));
    return float4((-mask >= 0) ? 0 : colour, mask);
#endif
}
