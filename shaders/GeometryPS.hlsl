// The dungeon's surfaces, patterned here from the world position (no texture memory): the
// colour's alpha picks the pattern. 1: plain, 0.75: floor stones (one a tile of 10 units),
// 0.5: wall bricks, 0.25: glowing (unlit), 0: a thing's own surface (patterned from where on
// the thing, so it moves with it): by the steps of 0.02 above it, a monster's skin with grime,
// wood, metal (with a shine) or stone. The floors and walls also carry the look of the
// stretch of floors they belong to (Theme, added to the alpha in steps of 0.02): 0 old bricks,
// 1 mossy stone ruins, 2 an ice cave, 3 a lava cave, 4 an amethyst abyss. Lit half by the
// scene's light, half ambient, and by a torch around the hero (TorchXZ, TorchRadius) that fades
// into darkness; a lava cave's cracks glow by themselves.
#include "Scene.hlsli"

struct VSOutput
{
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
    float3 normal : NORMAL;
    float4 color : COLOR0;
    float3 local : TEXCOORD1;
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

// Rougher noise: three octaves.
float Rough(float2 p)
{
    return Noise(p) * 0.55 + Noise(p * 2.1 + 3.7) * 0.3 + Noise(p * 4.3 + 9.1) * 0.15;
}

// Irregular stones (cells round scattered points): the distance to the nearest point and to
// the second nearest (their gap is the joint), and the nearest one's cell (its own shade).
float3 Cells(float2 p, out float2 id)
{
    const float2 i = floor(p), f = frac(p);
    float d1 = 8, d2 = 8;
    id = i;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            const float2 c = i + float2(x, y);
            const float2 point_ = float2(x, y) + 0.15 + 0.7 * float2(Hash(c), Hash(c + 41.7)) - f;
            const float d = length(point_);
            if (d < d1) { d2 = d1; d1 = d; id = c; }
            else if (d < d2) d2 = d;
        }
    return float3(d1, d2, d2 - d1);
}

// A thin line where the noise crosses its middle (cracks, veins): 1 on it, 0 away.
float Crack(float2 p, float width)
{
    return 1 - smoothstep(0.0, width, abs(Noise(p) - 0.5));
}

float3 Grey(float3 rgb, float amount)
{
    return lerp(rgb, dot(rgb, float3(0.3, 0.55, 0.15)).xxx, amount);
}

// The floor (and the ceiling seen from below) at xz; glow: light of its own.
float3 Floor(int theme, float3 rgb, float2 xz, inout float3 glow)
{
    if (theme == 1)
    {
        // Cobbles, worn and mossy between.
        float2 id;
        const float3 c = Cells(xz / 2.2, id);
        const float joint = 1 - smoothstep(0.04, 0.16, c.z);
        rgb *= (0.75 + 0.4 * Hash(id)) * (0.85 + 0.3 * Noise(xz * 1.3)) * lerp(0.35, 1, 1 - joint);
        rgb = Grey(rgb, 0.3);
        const float moss = smoothstep(0.45, 0.7, Noise(xz * 0.13 + 4.1));
        rgb = lerp(rgb, float3(0.18, 0.30, 0.12), joint * (0.4 + 0.5 * moss) + 0.25 * moss * (1 - joint));
        rgb *= 1 - 0.3 * smoothstep(0.5, 0.85, Noise(xz * 0.05));
    }
    else if (theme == 2)
    {
        // Plates of ice with pale cracks between, clouded here and there, glinting.
        float2 id;
        const float3 c = Cells(xz / 4.0, id);
        const float edge = 1 - smoothstep(0.02, 0.07, c.z);
        rgb *= 0.85 + 0.25 * Hash(id) + 0.15 * Noise(xz * 0.4);
        rgb = lerp(rgb, float3(0.95, 0.98, 1.0), 0.55 * edge + 0.25 * Crack(xz * 0.5, 0.02));
        rgb = lerp(rgb, rgb * float3(0.75, 0.85, 0.95), smoothstep(0.55, 0.8, Noise(xz * 0.09)));
        glow += float3(0.5, 0.6, 0.7) * step(0.985, Hash(floor(xz * 3.0))) * 0.6;
    }
    else if (theme == 3)
    {
        // Black rock in plates, the seams between them glowing with heat.
        float2 id;
        const float3 c = Cells(xz / 3.0, id);
        const float seam = 1 - smoothstep(0.02, 0.12, c.z);
        rgb *= (0.7 + 0.35 * Hash(id)) * (0.75 + 0.4 * Rough(xz * 0.8));
        rgb *= 1 - 0.5 * seam;
        const float heat = 0.55 + 0.45 * Noise(xz * 0.07 + 2.0);
        glow += float3(1.0, 0.32, 0.05) * seam * heat * 0.9;
    }
    else if (theme == 4)
    {
        // Marble tiles in a check, pale veins through them, thin gold seams.
        const float2 cell = xz / 5.0, local = frac(cell);
        const float edge = min(min(local.x, 1 - local.x), min(local.y, 1 - local.y));
        const float check = fmod(abs(floor(cell.x) + floor(cell.y)), 2);
        rgb *= lerp(0.55, 1.05, check);
        rgb = lerp(rgb, float3(0.85, 0.80, 0.95), 0.45 * Crack(xz * 0.35 + check * 7.0, 0.035));
        rgb = lerp(rgb, float3(0.80, 0.62, 0.30), 1 - smoothstep(0.0, 0.02, edge));
    }
    else
    {
        // Old slabs (the first floors).
        // A worn slab per tile: its own shade, a fine grain, darker toward its bevelled edge.
        const float2 cell = xz / 10;
        const float2 local = frac(cell);
        const float edge = min(min(local.x, 1 - local.x), min(local.y, 1 - local.y));
        rgb *= (0.9 + 0.2 * Hash(floor(cell))) * (0.86 + 0.24 * Noise(xz * 0.9)) * lerp(0.45, 1, smoothstep(0.0, 0.07, edge));
        // Faded and dirty as the walls: colour gone grey, broad stains, damp dark patches,
        // grime packed into the joints, and now and then moss in them.
        rgb = Grey(rgb, 0.4);
        const float stain = smoothstep(0.4, 0.85, Noise(xz * 0.05) * 0.6 + Noise(mul(float2x2(0.6, 0.8, -0.8, 0.6), xz) * 0.16) * 0.4);
        const float damp = smoothstep(0.62, 0.86, Noise(mul(float2x2(0.28, -0.96, 0.96, 0.28), xz) * 0.035 + 3.1));
        const float joint = 1 - smoothstep(0.0, 0.12, edge);
        const float moss = smoothstep(0.6, 0.78, Noise(xz * 0.11 + 11.7)) * joint;
        rgb *= (1 - 0.35 * stain) * (1 - 0.3 * damp) * (1 - 0.25 * joint);
        rgb = lerp(rgb, float3(0.20, 0.28, 0.15), 0.55 * moss);
    }
    return rgb;
}

// A wall at (along, height); glow: light of its own.
float3 Wall(int theme, float3 rgb, float along, float y, inout float3 glow)
{
    const float2 p = float2(along, y);
    if (theme == 1)
    {
        // Big rough-cut stone blocks, each course set off by its own amount, moss creeping up.
        const float course = floor(y / 2.6);
        const float shifted = along + Hash(float2(course, 3.3)) * 4.4;
        const float u = frac(shifted / 4.4), v = frac(y / 2.6);
        const float wobble = 0.03 * (Noise(p * 0.9) - 0.5);
        const float mortar = min(min(u, 1 - u) * 4.4, min(v, 1 - v) * 2.6) + wobble;
        rgb *= (0.75 + 0.4 * Hash(float2(floor(shifted / 4.4), course))) * (0.8 + 0.35 * Rough(p * 0.9)) * lerp(0.35, 1, smoothstep(0.04, 0.16, mortar));
        rgb = Grey(rgb, 0.3);
        const float moss = smoothstep(0.5, 0.75, Noise(p * 0.2 + 5.5)) * (1 - smoothstep(4.0, 22.0, y));
        rgb = lerp(rgb, float3(0.17, 0.30, 0.12), 0.7 * moss);
        rgb *= 1 - 0.3 * (1 - smoothstep(0.0, 3.0, y));
    }
    else if (theme == 2)
    {
        // Rough rock under ice: darker hollows, frosted pale toward the top, icicle streaks down it.
        const float n = Rough(p * 0.18);
        rgb *= 0.7 + 0.5 * n;
        rgb = lerp(rgb, float3(0.2, 0.3, 0.45), 0.4 * smoothstep(0.55, 0.8, Rough(p * 0.07 + 2.0)));
        const float frost = smoothstep(18.0, 38.0, y) * 0.5 + smoothstep(0.6, 0.85, Noise(float2(along * 0.9, y * 0.04))) * 0.4;
        rgb = lerp(rgb, float3(0.92, 0.96, 1.0), frost);
        rgb += 0.12 * smoothstep(0.7, 0.9, Noise(p * 0.6));
    }
    else if (theme == 3)
    {
        // Black basalt, its cracks glowing, hottest low down.
        rgb *= 0.6 + 0.6 * Rough(p * 0.22);
        const float crack = max(Crack(p * 0.16, 0.025), Crack(p * 0.31 + 5.0, 0.02) * 0.7);
        rgb *= 1 - 0.6 * crack;
        const float heat = (0.5 + 0.5 * Noise(p * 0.05)) * lerp(1.0, 0.35, smoothstep(0.0, 25.0, y));
        glow += float3(1.0, 0.30, 0.04) * crack * heat;
    }
    else if (theme == 4)
    {
        // Polished dark panels framed in gold, pale veins in the stone, a faint violet light
        // along the frames.
        const float u = frac(along / 5.0), v = frac(y / 5.0);
        const float frame = min(min(u, 1 - u), min(v, 1 - v));
        rgb *= 0.8 + 0.3 * Hash(float2(floor(along / 5.0), floor(y / 5.0)));
        rgb = lerp(rgb, float3(0.78, 0.70, 0.95), 0.4 * Crack(p * 0.3, 0.03));
        const float trim = 1 - smoothstep(0.015, 0.03, frame);
        rgb = lerp(rgb, float3(0.80, 0.62, 0.30), trim);
        glow += float3(0.45, 0.25, 0.8) * (1 - smoothstep(0.03, 0.08, frame)) * (1 - trim) * 0.35;
    }
    else
    {
        // Old bricks (the first floors).
        // Old bricks in courses, every other one shifted by half a brick; dark mortar between.
        const float course = floor(y / 1.6);
        const float u = frac((along + course * 1.6) / 3.2), v = frac(y / 1.6);
        const float mortar = min(min(u, 1 - u) * 3.2, min(v, 1 - v) * 1.6);
        rgb *= (0.86 + 0.28 * Hash(float2(floor((along + course * 1.6) / 3.2), course))) * (0.88 + 0.24 * Noise(p * 1.7)) *
               lerp(0.38, 1, smoothstep(0.03, 0.1, mortar));
        // Faded: much of the colour gone grey.
        rgb = Grey(rgb, 0.4);
        // Grime: broad dark stains, streaks run down from above, dirt gathered at the foot,
        // and here and there a green tinge of moss low down.
        const float stain = smoothstep(0.45, 0.8, Noise(p * 0.18) * 0.6 + Noise(p * 0.55) * 0.4);
        const float streak = smoothstep(0.55, 0.85, Noise(float2(along * 1.4, y * 0.06))) * smoothstep(0.0, 6.0, y);
        const float foot = 1 - smoothstep(0.0, 3.5, y);
        const float moss = smoothstep(0.62, 0.8, Noise(p * 0.35 + 7.3)) * (1 - smoothstep(1.0, 9.0, y));
        rgb *= (1 - 0.38 * stain) * (1 - 0.3 * streak) * (1 - 0.35 * foot);
        rgb = lerp(rgb, float3(0.22, 0.30, 0.16), 0.45 * moss);
    }
    return rgb;
}

float4 main(VSOutput i) : SV_Target
{
    const int pattern = (int)round(i.color.a * 4);
    const int theme = (int)round((i.color.a * 4 - pattern) / 0.08);
    float3 rgb = i.color.rgb, glow = 0;
    if (pattern == 1)
        return float4(rgb, 1);
    float shine = 0;
    if (pattern == 0 && theme == 1)
    {
        // Wood: a grain running along the boards, the boards' dark seams, a little grime.
        const float3 q = i.local;
        rgb *= 0.78 + 0.34 * Noise(float2(q.x * 0.22 + q.z * 0.05, (q.y + q.z) * 3.2));
        const float board = frac((q.y + 0.37 * q.z) / 1.15);
        rgb *= lerp(0.55, 1, smoothstep(0.0, 0.07, min(board, 1 - board)));
        rgb *= 1 - 0.3 * smoothstep(0.55, 0.85, Noise(q.xy * 0.5 + q.z * 0.3 + 2.0));
    }
    else if (pattern == 0 && theme == 2)
    {
        // Metal: a little uneven, scratched, and shining where the light glances off it.
        const float3 q = i.local;
        rgb *= 0.85 + 0.25 * Noise(q.xy * 1.8 + q.z * 1.3);
        rgb *= 1 - 0.25 * Crack(float2(q.x + q.z, q.y) * 1.4, 0.015);
        shine = 0.55;
    }
    else if (pattern == 0 && theme == 3)
    {
        // Stone: rough, mottled, with darker hollows.
        const float3 q = i.local;
        rgb *= 0.72 + 0.45 * Rough(q.xy * 0.7 + q.z * 0.45);
        rgb *= 1 - 0.3 * smoothstep(0.55, 0.8, Noise(q.zy * 0.4 + q.x * 0.3 + 6.0));
    }
    else if (pattern == 0)
    {
        // Grime: blotches of dirt, a fine grain, streaks run down, dirt up from the ground,
        // and the colour dulled a little.
        const float3 q = i.local;
        const float blotch = smoothstep(0.5, 0.8, Noise(q.xy * 0.45 + q.z * 0.31) * 0.6 + Noise(q.zy * 0.8 + q.x * 0.27 + 5.0) * 0.4);
        const float streak = smoothstep(0.62, 0.85, Noise(float2(q.x * 1.3 + q.z * 1.1, q.y * 0.12)));
        const float foot = 1 - smoothstep(0.0, 3.0, i.world.y);
        rgb *= 0.86 + 0.24 * Noise(q.xy * 2.2 + q.z * 1.7);
        rgb = Grey(rgb, 0.15);
        rgb = lerp(rgb, rgb * float3(0.55, 0.45, 0.35), 0.55 * blotch + 0.3 * streak);
        rgb = lerp(rgb, float3(0.16, 0.12, 0.09), 0.5 * foot);
    }
    else if (pattern == 3)
        rgb = Floor(theme, rgb, i.world.xz, glow);
    else if (pattern == 2)
        rgb = Wall(theme, rgb, abs(i.normal.x) > 0.5 ? i.world.z : i.world.x, i.world.y, glow);
    const float lit = saturate(dot(normalize(i.normal), -normalize(LightDir)));
    float light = 0.55 + 0.6 * lit;
    if (TorchRadius > 0)
    {
        const float d = distance(i.world.xz, TorchXZ);
        light *= lerp(0.22, 1.15, pow(saturate(1 - d / TorchRadius), 1.4));
    }
    if (shine > 0)
    {
        // A highlight where the light would glance off toward the camera.
        const float3 view = normalize(cross(CameraRight.xyz, CameraUp.xyz));
        const float3 bounce = reflect(normalize(LightDir), normalize(i.normal));
        glow += shine * pow(saturate(dot(bounce, -view)), 18) * light;
    }
    return float4(rgb * light + glow, 1);
}
