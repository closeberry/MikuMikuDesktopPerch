// The dungeon's surfaces, patterned here from the world position (no texture memory): the
// colour's alpha picks the pattern. 1: plain, 0.75: floor stones (one a tile of 10 units),
// 0.5: wall bricks, 0.25: glowing (unlit). Lit half by the scene's light, half ambient, and
// by a torch around the hero (TorchXZ, TorchRadius) that fades into darkness.
#include "Scene.hlsli"

struct VSOutput
{
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
    float3 normal : NORMAL;
    float4 color : COLOR0;
};

float Hash(float2 p)
{
    return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);
}

float Noise(float2 p)
{
    const float2 i = floor(p), f = frac(p);
    const float2 u = f * f * (3 - 2 * f);
    return lerp(lerp(Hash(i), Hash(i + float2(1, 0)), u.x), lerp(Hash(i + float2(0, 1)), Hash(i + float2(1, 1)), u.x), u.y);
}

float4 main(VSOutput i) : SV_Target
{
    const int pattern = (int)round(i.color.a * 4);
    float3 rgb = i.color.rgb;
    if (pattern == 1)
        return float4(rgb, 1);
    if (pattern == 3)
    {
        // A worn slab per tile: its own shade, a fine grain, darker toward its bevelled edge.
        const float2 cell = i.world.xz / 10;
        const float2 local = frac(cell);
        const float edge = min(min(local.x, 1 - local.x), min(local.y, 1 - local.y));
        rgb *= (0.9 + 0.2 * Hash(floor(cell))) * (0.86 + 0.24 * Noise(i.world.xz * 0.9)) * lerp(0.45, 1, smoothstep(0.0, 0.07, edge));
    }
    else if (pattern == 2)
    {
        // Bricks in courses, every other one shifted by half a brick; dark mortar between.
        const float along = abs(i.normal.x) > 0.5 ? i.world.z : i.world.x;
        const float course = floor(i.world.y / 1.2);
        const float u = frac((along + course * 1.25) / 2.5), v = frac(i.world.y / 1.2);
        const float mortar = min(min(u, 1 - u) * 2.5, min(v, 1 - v) * 1.2);
        rgb *= (0.88 + 0.24 * Hash(float2(floor((along + course * 1.25) / 2.5), course))) * (0.9 + 0.2 * Noise(float2(along, i.world.y) * 1.7)) *
               lerp(0.35, 1, smoothstep(0.03, 0.09, mortar));
    }
    const float lit = saturate(dot(normalize(i.normal), -normalize(LightDir)));
    float light = 0.55 + 0.6 * lit;
    if (TorchRadius > 0)
    {
        const float d = distance(i.world.xz, TorchXZ);
        light *= lerp(0.22, 1.15, pow(saturate(1 - d / TorchRadius), 1.4));
    }
    return float4(rgb * light, 1);
}
