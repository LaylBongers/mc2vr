// The game's two-channel tangent-space normal maps (normalMap; docs/shaders.md):
// x in alpha, y in green, z reconstructed. Shared by the PgDiff*Norm families
// and PgDiffRefractNormFP.
//
// A deliberate fix in both shadings. Every original that reads a normalMap
// (all 119 in shader3.bin and all 68 in shader3Low.bin) rebuilds z as
// sqrt(|1 - x² - 2y|): y unsquared, a slip in the game's source. Against the
// true sqrt(1 - x² - y²) this over-tilts every texel leaning +y (z too small,
// and past y = (1 - x²)/2 the absolute value of a negative goes back up) and
// flattens every texel leaning -y (z too large). Here z is the unit vector's,
// 0 outside the unit disc (compression overshoot). tools/shader_equiv.py
// corrects the originals the same way before comparing.

#ifndef MC2VR_NORMAL_MAP_HLSLI
#define MC2VR_NORMAL_MAP_HLSLI

// The tangent-space normal from a normalMap texel (unit inside the disc).
float3 tangent_space_normal(float4 texel)
{
    const float2 xy = 2 * (texel.wy - 0.5);
    return float3(xy, sqrt(saturate(1 - xy.x * xy.x - xy.y * xy.y)));
}

// The texel's normal in the interpolated tangent frame, normalized.
float3 mapped_normal(float4 texel, float3 tangent, float3 binormal, float3 normal)
{
    const float3 n = tangent_space_normal(texel);
    return normalize(n.x * tangent + n.y * binormal + n.z * normal);
}

#endif
