// Shaped pieces drawn many times over (the chess men, MascotRenderer::SetPieces): one mesh per
// shape, each drawn with its own place in the world and its own neck. A vertex follows the body
// and the neck by its weight, as a PMX vertex follows two bones.
#include "Scene.hlsli"

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float4 color : COLOR;
    float neck : TEXCOORD1;
    float4 world0 : TEXCOORD3;
    float4 world1 : TEXCOORD4;
    float4 world2 : TEXCOORD5;
    float4 neck0 : TEXCOORD6;
    float4 neck1 : TEXCOORD7;
    float4 neck2 : TEXCOORD8;
    float4 tint : TEXCOORD9;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
    float3 local : TEXCOORD1;   // where on the man itself (its engraving moves with it)
    float3 normal : NORMAL;
    float4 color : COLOR0;
    float4 tint : COLOR1;
};

VSOutput main(VSInput i)
{
    VSOutput o;
    const float4 p = float4(i.position, 1);
    const float3 bent = float3(dot(i.neck0, p), dot(i.neck1, p), dot(i.neck2, p));
    const float3 n = i.normal;
    const float3 bentN = float3(dot(i.neck0.xyz, n), dot(i.neck1.xyz, n), dot(i.neck2.xyz, n));
    const float4 q = float4(lerp(i.position, bent, i.neck), 1);
    const float3 m = lerp(n, bentN, i.neck);
    o.world = float3(dot(i.world0, q), dot(i.world1, q), dot(i.world2, q));
    o.normal = float3(dot(i.world0.xyz, m), dot(i.world1.xyz, m), dot(i.world2.xyz, m));
    o.position = mul(float4(o.world, 1), ParticleViewProj);
    o.local = i.position;
    o.color = i.color;
    o.tint = i.tint;
    return o;
}
