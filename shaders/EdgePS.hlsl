#include "Common.hlsli"

struct PSInput
{
    float4 position : SV_Position;
    float4 clipNow : TEXCOORD4;
    float4 clipPrev : TEXCOORD5;
};

struct PSOutput
{
    float4 color : SV_Target0;
    float2 motion : SV_Target1;
};

PSOutput main(PSInput input)
{
    clip(EdgeColor.a - 1.0 / 255.0);
    PSOutput output;
    output.color = EdgeColor;
    output.motion = ScreenMotion(input.clipNow, input.clipPrev);
    return output;
}
