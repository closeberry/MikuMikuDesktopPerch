#include "Common.hlsli"

float4 main() : SV_Target
{
    clip(EdgeColor.a - 1.0 / 255.0);
    return EdgeColor;
}
