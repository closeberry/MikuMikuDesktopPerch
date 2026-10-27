// Plain lit geometry in world space (the dungeon's floor, walls and things): no textures, a
// colour per vertex (its alpha says which pattern the pixel shader draws).
#include "Scene.hlsli"

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float4 color : COLOR;
    float4 wall : TEXCOORD1;   // a wall top: its tile's middle (x, y), how far it sinks (z), floor straight beyond it (w); on a monster, where on it (x, y, w)
    float4 uv : TEXCOORD2;     // where on the geometry's texture (x, y), 1 in z when it is drawn there
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
    float3 normal : NORMAL;
    float4 color : COLOR0;
    float3 local : TEXCOORD1;   // where on a monster (its grime's pattern)
    float3 uv : TEXCOORD2;
};

static const float Tile = 10;   // a tile's side (DungeonWindow's TileSize)

// How far (0 to 1) a wall sinks so it does not hide the hero (TorchXZ, where she is drawn, so
// it eases as she walks), seen from above: one on the camera's side of her that stands
// straight in front of floor, or one in her own column within 3 tiles. From behind her the
// walls stand (the camera comes in short of a wall instead), so it eases to 0 as the camera
// moves there (by how upright it is).
float Sink(float2 middle, float floorBeyond)
{
    const float2 right = normalize(CameraRight.xz);
    const float2 forward = float2(-right.y, right.x);   // the way the camera looks, on the ground
    const float2 d = (middle - TorchXZ) / Tile;
    const float toward = dot(d, -forward);               // tiles toward the camera from her
    const float aside = abs(dot(d, right));
    const float before = smoothstep(0.3, 0.8, toward);
    const float above = max(floorBeyond, (1 - smoothstep(0.55, 0.95, aside)) * (1 - smoothstep(3.0, 3.6, toward))) * before;
    return above * (1 - smoothstep(0.8, 0.95, CameraUp.y));
}

VSOutput main(VSInput i)
{
    VSOutput o;
    float3 p = i.position;
    if (i.wall.z > 0)
        p.y -= i.wall.z * Sink(i.wall.xy, i.wall.w);
    o.position = mul(float4(p, 1), ParticleViewProj);
    o.world = p;
    o.normal = i.normal;
    o.color = i.color;
    o.local = i.wall.xyw;
    o.uv = i.uv.xyz;
    return o;
}
