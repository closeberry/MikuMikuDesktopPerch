// The scenery's instanced things (the golf course's trees): one mesh drawn at many places,
// each turned about the up axis and sized (MascotRenderer::SetSceneryInstances). Otherwise as
// GeometryVS (no sinking walls).
#include "Scene.hlsli"

struct Instance
{
    float4 place;   // x, y, z (world), turn about the up axis (radians)
    float4 size;    // x: how much bigger than the mesh
};
StructuredBuffer<Instance> Instances : register(t0);

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float4 color : COLOR;
    float4 wall : TEXCOORD1;
    float4 uv : TEXCOORD2;
    uint instance : SV_InstanceID;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
    float3 normal : NORMAL;
    float4 color : COLOR0;
    float3 local : TEXCOORD1;
    float4 uv : TEXCOORD2;
};

VSOutput main(VSInput i)
{
    const Instance k = Instances[i.instance];
    const float c = cos(k.place.w), s = sin(k.place.w);
    const float3 p0 = i.position * k.size.x;
    const float3 p = float3(p0.x * c + p0.z * s, p0.y, -p0.x * s + p0.z * c) + k.place.xyz;
    VSOutput o;
    o.position = mul(float4(p, 1), ParticleViewProj);
    o.world = p;
    o.normal = float3(i.normal.x * c + i.normal.z * s, i.normal.y, -i.normal.x * s + i.normal.z * c);
    o.color = i.color;
    o.local = i.wall.xyw;
    o.uv = i.uv;
    return o;
}
