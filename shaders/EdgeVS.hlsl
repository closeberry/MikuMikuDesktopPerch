#include "Common.hlsli"

float4 main(VSInput input) : SV_Position
{
    float3 position, normal;
    Skin(input, position, normal);
    float4 clipPosition = mul(float4(position, 1), ViewProj);
    float2 clipNormal = mul(float4(normal, 0), ViewProj).xy;
    float len = length(clipNormal);
    if (len > 1e-6)
    {
        // Screen-constant outline: EdgePixels * edge size * per-vertex edge scale.
        float2 offset = clipNormal / len * EdgeSize * input.edge * EdgePixels * 2.0 / ViewportSize;
        clipPosition.xy += offset * clipPosition.w;
    }
    return clipPosition;
}
