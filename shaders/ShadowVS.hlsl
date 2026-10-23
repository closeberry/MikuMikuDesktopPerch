#include "Common.hlsli"

// Self-shadow map: the model's depth as seen from the light.
struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput main(VSInput input)
{
    float3 position, normal;
    Skin(input, position, normal);
    VSOutput output;
    output.position = mul(float4(position, 1), LightViewProj);
    output.uv = input.uv;
    return output;
}
