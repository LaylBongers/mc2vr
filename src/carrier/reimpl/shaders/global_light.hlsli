// The game's global light record (GlobalLightData_Build; docs/shadows.md):
// shared by the reimplemented receivers in this directory.
//
// globalLightData (c9..c34): [0] -light direction, [1] light colour,
// [3..8] the ambient cube (+x, -x, +y, -y, +z, -z colours), [9] the shadow
// fade (built into the VS's fade output), [10] the cascades' cumulative split
// ends, [11 + 4i ..] cascade i's world -> atlas matrix rows (u, v, depth;
// 4 rows, 3 used).

#ifndef MC2VR_GLOBAL_LIGHT_HLSLI
#define MC2VR_GLOBAL_LIGHT_HLSLI

float4 globalLightData[26] : register(c9);

// The ambient cube at a unit normal: per axis the colour on the normal's
// side, weighted by the normal's squared component.
float3 ambient_cube(float3 n)
{
    const float3 n2 = n * n;
    const float3 x = (n.x >= 0) ? globalLightData[3].rgb : globalLightData[4].rgb;
    const float3 y = (n.y >= 0) ? globalLightData[5].rgb : globalLightData[6].rgb;
    const float3 z = (n.z >= 0) ? globalLightData[7].rgb : globalLightData[8].rgb;
    return x * n2.x + y * n2.y + z * n2.z;
}

#endif
