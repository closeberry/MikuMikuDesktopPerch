// The frame drawn at a lower resolution, scaled up to the window's pixels (bilinear).
#include "Scene.hlsli"

Texture2D<float4> Source : register(t1);   // premultiplied
SamplerState ClampSampler : register(s1);

float4 main(float4 position : SV_Position) : SV_Target
{
    // BlurInfo.zw: drawn pixels per window pixel. Kept inside the drawn area, whose outside
    // may still hold an older, larger frame.
    float w, h;
    Source.GetDimensions(w, h);
    const float2 p = clamp(position.xy * BlurInfo.zw, 0.5, ViewportSize - 0.5);
    return Source.SampleLevel(ClampSampler, p / float2(w, h), 0);
}
