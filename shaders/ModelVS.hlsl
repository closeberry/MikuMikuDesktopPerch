#include "Common.hlsli"

struct VSOutput
{
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float3 viewNormal : TEXCOORD1;
    float3 worldPosition : TEXCOORD2;
    float2 uv : TEXCOORD0;
    float4 shadowPosition : TEXCOORD3;
};

VSOutput main(VSInput input)
{
    float3 position, normal;
    Skin(input, position, normal);
    VSOutput output;
    output.position = mul(float4(position, 1), ViewProj);
    output.normal = normal;
    output.viewNormal = mul(normal, (float3x3)View);
    output.worldPosition = position;
    output.uv = input.uv;
    output.shadowPosition = mul(float4(position, 1), LightViewProj);
    return output;
}
