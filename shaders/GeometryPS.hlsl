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

// Gradient noise (0 to 1), turned off the axes: value noise flattens along its grid lines, and
// the stains' thresholds showed those as straight lines.
float2 Gradient(float2 cell)
{
    const float a = Hash(cell) * 6.2831853;
    return float2(cos(a), sin(a));
}

float Noise(float2 p)
{
    p = mul(float2x2(0.8, -0.6, 0.6, 0.8), p) + float2(17.3, 5.9);
    const float2 i = floor(p), f = frac(p);
    const float2 u = f * f * f * (f * (f * 6 - 15) + 10);
    const float a = dot(Gradient(i), f), b = dot(Gradient(i + float2(1, 0)), f - float2(1, 0));
    const float c = dot(Gradient(i + float2(0, 1)), f - float2(0, 1)), d = dot(Gradient(i + 1), f - 1);
    return saturate(0.5 + 0.75 * lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y));
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
        // Faded and dirty as the walls: colour gone grey, broad stains, damp dark patches,
        // grime packed into the joints, and now and then moss in them.
        rgb = lerp(rgb, dot(rgb, float3(0.3, 0.55, 0.15)).xxx, 0.4);
        const float stain = smoothstep(0.4, 0.85, Noise(i.world.xz * 0.05) * 0.6 + Noise(mul(float2x2(0.6, 0.8, -0.8, 0.6), i.world.xz) * 0.16) * 0.4);
        const float damp = smoothstep(0.62, 0.86, Noise(mul(float2x2(0.28, -0.96, 0.96, 0.28), i.world.xz) * 0.035 + 3.1));
        const float joint = 1 - smoothstep(0.0, 0.12, edge);
        const float moss = smoothstep(0.6, 0.78, Noise(i.world.xz * 0.11 + 11.7)) * joint;
        rgb *= (1 - 0.35 * stain) * (1 - 0.3 * damp) * (1 - 0.25 * joint);
        rgb = lerp(rgb, float3(0.20, 0.28, 0.15), 0.55 * moss);
    }
    else if (pattern == 2)
    {
        // Old bricks in courses, every other one shifted by half a brick; dark mortar between.
        const float along = abs(i.normal.x) > 0.5 ? i.world.z : i.world.x;
        const float course = floor(i.world.y / 1.6);
        const float u = frac((along + course * 1.6) / 3.2), v = frac(i.world.y / 1.6);
        const float mortar = min(min(u, 1 - u) * 3.2, min(v, 1 - v) * 1.6);
        rgb *= (0.86 + 0.28 * Hash(float2(floor((along + course * 1.6) / 3.2), course))) * (0.88 + 0.24 * Noise(float2(along, i.world.y) * 1.7)) *
               lerp(0.38, 1, smoothstep(0.03, 0.1, mortar));
        // Faded: much of the colour gone grey.
        rgb = lerp(rgb, dot(rgb, float3(0.3, 0.55, 0.15)).xxx, 0.4);
        // Grime: broad dark stains, streaks run down from above, dirt gathered at the foot,
        // and here and there a green tinge of moss low down.
        const float stain = smoothstep(0.45, 0.8, Noise(float2(along, i.world.y) * 0.18) * 0.6 + Noise(float2(along, i.world.y) * 0.55) * 0.4);
        const float streak = smoothstep(0.55, 0.85, Noise(float2(along * 1.4, i.world.y * 0.06))) * smoothstep(0.0, 6.0, i.world.y);
        const float foot = 1 - smoothstep(0.0, 3.5, i.world.y);
        const float moss = smoothstep(0.62, 0.8, Noise(float2(along, i.world.y) * 0.35 + 7.3)) * (1 - smoothstep(1.0, 9.0, i.world.y));
        rgb *= (1 - 0.38 * stain) * (1 - 0.3 * streak) * (1 - 0.35 * foot);
        rgb = lerp(rgb, float3(0.22, 0.30, 0.16), 0.45 * moss);
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
