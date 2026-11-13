// tools/test/test_hlsl.cpp's shader: the game's static light-count branches
// (numPointLights in b0-b3, `if bN`, as the originals), which a compiler that
// maps bools to float registers (vkd3d-shader) gets wrong.
#include "static_bool_common.hlsli"

bool numPointLights[4] : register(b0);
float4 pointLights[8] : register(c36);

float4 main(float4 c : TEXCOORD0) : COLOR
{
    float3 l = c.rgb;
    if (numPointLights[1]) l += pointLights[1].rgb;
    if (numPointLights[0]) l += pointLights[0].rgb;
    return float4(clamp_hdr(l), 1);
}
