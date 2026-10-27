// A picture laid over the drawn frame (SetOverlay: premultiplied, the drawn size): the mahjong
// table's hand, buttons and words over the table.
#include "Scene.hlsli"

Texture2D<float4> Overlay : register(t2);
SamplerState ClampSampler : register(s1);

float4 main(float4 position : SV_Position) : SV_Target
{
    return Overlay.SampleLevel(ClampSampler, position.xy / ViewportSize, 0);
}
