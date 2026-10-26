// Plain lit geometry in world space (the dungeon's floor, walls and things): no textures, a
// colour per vertex (its alpha says which pattern the pixel shader draws).
#include "Scene.hlsli"

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float4 color : COLOR;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
    float3 normal : NORMAL;
    float4 color : COLOR0;
};

VSOutput main(VSInput i)
{
    VSOutput o;
    o.position = mul(float4(i.position, 1), ParticleViewProj);
    o.world = i.position;
    o.normal = i.normal;
    o.color = i.color;
    return o;
}
