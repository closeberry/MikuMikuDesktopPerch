// The dungeon's surfaces (and the mahjong table's), patterned here from the world position (no texture memory
// but the mahjong tiles' faces, Picture, where the vertices ask for it): the
// colour's alpha picks the pattern. 1: plain, 0.75: floor stones (one a tile of 10 units),
// 0.5: wall bricks, 0.25: glowing (unlit), 0: a thing's own surface (patterned from where on
// the thing, so it moves with it): by the steps of 0.02 above it, a monster's skin with grime,
// wood, metal (with a shine) or stone; look 6: the Western parlour's surfaces (Parlour, Gilt, CutGlass). The floors and walls also carry the look of the
// stretch of floors they belong to (Theme, added to the alpha in steps of 0.02): 0 old bricks,
// 1 mossy stone ruins, 2 an ice cave, 3 a lava cave, 4 an amethyst abyss. Lit half by the
// scene's light, half ambient, and by a torch around the hero (TorchXZ, TorchRadius) that fades
// into darkness, and by fire and magic nearby (PointLights); a lava cave's cracks glow by themselves.
// On a flooded floor (Water) the floors are under shallow water with rings spreading on it
// (Flooded), and the walls are wet and dark at their feet.
#include "Scene.hlsli"

struct VSOutput
{
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
    float3 normal : NORMAL;
    float4 color : COLOR0;
    float3 local : TEXCOORD1;
    float4 uv : TEXCOORD2;     // where on Picture, 1 in z when it is drawn there; how see-through in w (0: solid)
};

Texture2D<float4> Picture : register(t1);   // the geometry's texture (the mahjong tiles' faces)
SamplerState ClampSampler : register(s1);
SamplerState WrapSampler : register(s0);   // the scenery's tiled pictures (2 in uv.z)

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

// Shallow water over a floor at world: the floor seen through it (darker, tinted), small waves
// always moving, rings spreading from each ripple, and the light glinting off it (her torch
// and the fire and magic nearby) where the waves turn it toward the camera.
float3 Flooded(float3 floorColour, float3 light, float3 glow, float3 world)
{
    const float t = Water.y;
    const float2 p = world.xz;
    float2 slope = 0.045 * float2(cos(p.x * 0.9 + t * 1.3) + 0.6 * cos((p.x + p.y) * 0.7 - t * 1.1),
                                  cos(p.y * 0.8 - t * 1.2) + 0.6 * cos((p.x - p.y) * 0.6 + t * 0.9));
    float ring = 0;
    [unroll] for (int k = 0; k < 16; ++k)
    {
        const float4 r = Ripples[k];
        if (r.w <= 0)
            continue;
        // A train of a few rings, its front spreading 9 units a second, weaker as it grows and
        // as it ages. Not perfectly round: the ring is pushed out and in around it and warped by
        // the water where it runs (more as it grows), and stronger on some sides than others
        // (each ripple its own way, from where it began).
        const float2 d = p - r.xy;
        const float dist = length(d) + 1e-3;
        // Around the ring (by its angle a): a few soft waves of its own, from where it began.
        const float a = atan2(d.y, d.x);
        const float s1 = Hash(r.xy) * 6.283, s2 = Hash(r.yx + 3.1) * 6.283, s3 = Hash(r.xy + 7.7) * 6.283;
        const float front = r.z * 9.0;
        const float near = saturate(dist / 4.0);   // none of it at the very middle
        const float warp = near * (0.08 + 0.025 * front) * (sin(2 * a + s1) + 0.6 * sin(3 * a + s2))
                         + (Noise(p * 0.35 + r.xy * 0.13 + t * 0.2) - 0.5) * 0.7;
        const float x = dist + warp - front;
        const float sides = 1 - 0.45 * near * (0.5 + 0.5 * (0.7 * sin(a + s3) + 0.3 * sin(2 * a + s1)));
        const float envelope = r.w * sides * exp(-x * x / 5.0) * saturate(1 - r.z / 2.2) / (1 + 0.12 * front);
        // The rings not evenly spaced: each train its own spacing, and the gaps within it
        // stretching and closing a little.
        const float phase = x * (2.2 + 0.9 * Hash(r.yx + 1.9)) + 0.9 * sin(x * 0.8 + s2);
        slope += d / dist * cos(phase) * envelope;
        ring += sin(phase) * envelope;
    }
    const float3 n = normalize(float3(-slope.x, 1, -slope.y));
    const float3 view = normalize(cross(CameraRight.xyz, CameraUp.xyz));
    const float facing = saturate(dot(n, -view));
    const float fresnel = 0.12 + 0.55 * pow(1 - facing, 3);
    const float3 under = floorColour * light * float3(0.6, 0.7, 0.76);
    const float3 above = float3(0.08, 0.1, 0.14) * light;
    // The rings' crests catch a little light and their troughs darken, wherever they are.
    float3 c = lerp(under, above, fresnel) * (1 - 0.25 * saturate(-ring)) + saturate(ring) * 0.32 * light * float3(0.85, 0.92, 1.0);
    // Her torch, held about her height, mirrored in the water.
    if (TorchRadius > 0)
    {
        const float3 l = normalize(float3(TorchXZ.x, 12, TorchXZ.y) - world);
        const float fade = saturate(1 - distance(p, TorchXZ) / TorchRadius);
        c += float3(1.0, 0.95, 0.85) * pow(saturate(dot(n, normalize(l - view))), 70) * 0.9 * fade;
    }
    [unroll] for (int j = 0; j < 8; ++j)
    {
        const float4 q = LightPlaces[j];
        if (q.w <= 0)
            continue;
        const float3 l = normalize(q.xyz - world);
        const float fade = saturate(1 - distance(q.xyz, world) / (q.w * 1.5));
        c += LightColors[j].rgb * pow(saturate(dot(n, normalize(l - view))), 50) * 1.2 * fade;
    }
    return c + glow;
}

// Seigaiha (the waves: rows of fans of rings, each row half a fan across from the one above,
// the lower in front), in fans one unit across: how near p is to a ring's line (1 on it).
float Seigaiha(float2 p)
{
    const float r = 0.5;
    const float w = (fwidth(p.x) + fwidth(p.y)) * 4 / r + 0.08;   // the line's width, kept from flickering far off
    const int row0 = (int)floor((p.y - r) / (r * 0.5));
    float ring = 0;
    bool found = false;
    [unroll] for (int k = 0; k <= 4; ++k)
    {
        const int row = row0 + k;
        const float cy = row * r * 0.5;
        const float shift = (row & 1) ? r : 0;
        const float cx = floor((p.x - shift) / (2 * r) + 0.5) * 2 * r + shift;
        const float d = distance(p, float2(cx, cy));
        if (!found && d < r && p.y >= cy)
        {
            const float t = d / r * 4;
            ring = 1 - smoothstep(0, w, abs(t - round(t)));
            found = true;
        }
    }
    return ring;
}

// The Western parlour's surfaces (WesternRoom.cpp; the kind in minus uv's z): at (q.x, q.y).
float3 Damask(float3 rgb, float3 q)
{
    // Damask: a half-drop repeat of flowers in ogee frames, tone on tone.
    float2 c = q.xy * float2(3.4, 2.5);
    c.x += 0.5 * floor(c.y);
    float2 p = frac(c) - 0.5;
    p.x = abs(p.x);
    const float r = length(p * float2(1.5, 1.0));
    const float a = atan2(p.x, p.y);
    const float petals = 0.5 + 0.5 * cos(a * 5);
    const float flower = smoothstep(0.27, 0.25, r * (0.75 + 0.5 * petals)) * smoothstep(0.07, 0.09, r);
    const float frame = 1 - smoothstep(0.0, 0.035, abs(p.x + 0.72 * abs(p.y) * abs(p.y) * 2 - 0.46));
    const float aa = saturate(0.5 / (0.001 + 40 * fwidth(q.y)));
    return rgb * (0.88 + aa * (0.22 * flower + 0.18 * frame)) * (0.96 + 0.08 * Noise(q.xy * 24));
}

float3 ParquetFloor(float3 rgb, float3 q)
{
    // Parquet: chevrons of oak strips, each strip its own shade with a grain along it.
    const float2 c = q.xy * 2.4;
    const float col = floor(c.x);
    const float x = frac(c.x) - 0.5;
    const float way = fmod(abs(col), 2) < 0.5 ? 1.0 : -1.0;
    const float y = (c.y + way * x * 0.9) * 5;
    const float strip = floor(y);
    const float across = frac(y);
    const float seam = smoothstep(0.0, 0.06, min(across, 1 - across)) * smoothstep(0.0, 0.025, 0.5 - abs(x));
    const float shade = 0.80 + 0.36 * Hash(float2(col, strip));
    const float grain_ = 0.88 + 0.24 * Noise(float2((c.x - way * c.y) * 2.0 + strip * 3.1, y * 8.0));
    return rgb * shade * grain_ * lerp(0.42, 1, seam);
}

float3 RugPattern(float3 rgb, float3 q)
{
    // A rug: a dark red field and a medallion inside a navy border with pale lines.
    const float2 p = q.xy;
    const float edge = max(abs(p.x), abs(p.y));
    const float3 red = float3(0.34, 0.055, 0.06), navy = float3(0.055, 0.075, 0.17), gold = float3(0.56, 0.42, 0.20);
    float3 c = red;
    const float small_ = 0.5 + 0.5 * cos(p.x * 46) * cos(p.y * 46);
    c = lerp(c, c * 1.35 + gold * 0.05, 0.6 * small_);
    const float r = length(p * float2(1.15, 1.0));
    const float a = atan2(p.y, p.x);
    const float rim = 0.40 + 0.045 * cos(a * 16);
    c = lerp(c, navy, smoothstep(rim + 0.008, rim, r));
    c = lerp(c, gold, 1 - smoothstep(0.0, 0.012, abs(r - rim)));
    c = lerp(c, red * 1.3, smoothstep(0.23, 0.22, r + 0.02 * cos(a * 8)));
    c = lerp(c, gold, 1 - smoothstep(0.0, 0.012, abs(r - 0.12)));
    if (edge > 0.74)
    {
        c = navy * (0.8 + 0.5 * step(0.5, frac((abs(p.x) + abs(p.y)) * 14)));
        c = lerp(c, gold, 1 - smoothstep(0.0, 0.010, abs(edge - 0.86)));
    }
    c = lerp(c, gold, 1 - smoothstep(0.0, 0.009, abs(edge - 0.74)));
    c = lerp(c, float3(0.62, 0.56, 0.44), smoothstep(0.975, 0.98, edge));
    return c * (0.88 + 0.22 * Noise(p * 90));
}

float3 PaintingPattern(float3 rgb, float3 q)
{
    // A painting: hills under an evening sky, in dabs of oil.
    const float2 p = q.xy + 0.012 * (float2(Noise(q.xy * 40), Noise(q.yx * 37)) - 0.5);
    float3 c = lerp(float3(0.92, 0.60, 0.32), float3(0.24, 0.32, 0.50), smoothstep(0.42, 0.95, p.y));
    c = lerp(c, float3(1.0, 0.86, 0.60), smoothstep(0.34, 0.0, length((p - float2(0.68, 0.56)) * float2(1.0, 1.7))));
    c = lerp(c, float3(0.70, 0.46, 0.42), 0.55 * smoothstep(0.52, 0.72, Noise(p * float2(5, 16) + 3)) * smoothstep(0.45, 0.6, p.y));
    const float far_ = 0.50 + 0.07 * Noise(float2(p.x * 5, 1.5));
    c = lerp(c, float3(0.30, 0.31, 0.42), step(p.y, far_));
    const float mid = 0.37 + 0.09 * Noise(float2(p.x * 4 + 7, 2.5));
    c = lerp(c, float3(0.20, 0.27, 0.17) * (0.8 + 0.5 * Noise(p * 22)), step(p.y, mid));
    const float near_ = 0.19 + 0.07 * Noise(float2(p.x * 7 + 3, 4.5));
    c = lerp(c, float3(0.11, 0.14, 0.07) * (0.7 + 0.8 * Noise(p * 30)), step(p.y, near_));
    const float tree = smoothstep(0.17, 0.15, length((p - float2(0.2, 0.42)) * float2(1.3, 1.0)) + 0.06 * Noise(p * 26));
    c = lerp(c, float3(0.08, 0.12, 0.06) * (0.7 + 0.8 * Noise(p * 44)), tree);
    return c * (0.92 + 0.16 * Noise(p * 120));
}

float3 Curtains(float3 rgb, float3 q)
{
    // Curtains: heavy folds, velvet at their edges.
    const float folds = 0.5 + 0.5 * cos(q.x * 34 + 0.7 * sin(q.y * 1.5));
    return rgb * (0.42 + 0.9 * pow(folds, 1.4)) * (0.95 + 0.1 * Noise(q.xy * 30));
}

float3 Parlour(int kind, float3 rgb, float3 q)
{
    float3 c;
    if (kind == 1)
        c = Damask(rgb, q);
    else if (kind == 2)
        c = ParquetFloor(rgb, q);
    else if (kind == 3)
        c = RugPattern(rgb, q);
    else if (kind == 4)
        c = PaintingPattern(rgb, q);
    else
        c = Curtains(rgb, q);
    return c;
}

// The parlour as a polished thing mirrors it in direction r: the dark floor, warm walls with the
// windows and flames as bright patches, the pale ceiling.
float3 ParlourMirrored(float3 r)
{
    float3 c = r.y < 0 ? lerp(float3(0.30, 0.16, 0.09), float3(0.09, 0.05, 0.03), saturate(-r.y * 1.8))
                       : lerp(float3(0.40, 0.28, 0.19), float3(0.95, 0.85, 0.66), saturate(r.y * 1.5));
    const float around = atan2(r.x, r.z);
    c += float3(1.25, 1.0, 0.68) * pow(saturate(0.5 + 0.5 * cos(around * 6)), 24) * smoothstep(0.6, 0.1, abs(r.y - 0.15));
    return c;
}

float3 FacetNormal(float3 world, float3 n)
{
    const float3 f = normalize(cross(ddx(world), ddy(world)));
    return dot(f, n) < 0 ? -f : f;
}

// Polished gold: nearly all mirror, its colour being what it mirrors of each light.
float3 Gilt(float3 rgb, float3 world, float3 smoothNormal)
{
    const float3 forward = normalize(cross(CameraRight.xyz, CameraUp.xyz));
    const float3 n = normalize(lerp(smoothNormal, FacetNormal(world, smoothNormal), 0.35));
    const float facing = saturate(dot(n, -forward));
    const float3 mirror = rgb + (1 - rgb) * 0.45 * pow(1 - facing, 5);
    const float3 half_ = normalize(-normalize(LightDir) - forward);
    return min(ParlourMirrored(reflect(forward, n)), 1.15) * mirror + float3(1.0, 0.93, 0.78) * pow(saturate(dot(n, half_)), 90) * 1.2;
}

// Cut glass: dark and clear inside with what is bright round it bent into streaks, bright at its
// edges, and each facet its own: some catch the light whole, split into colours.
float3 CutGlass(float3 rgb, float3 world, float3 smoothNormal)
{
    const float3 forward = normalize(cross(CameraRight.xyz, CameraUp.xyz));
    const float3 n = FacetNormal(world, smoothNormal);
    const float facing = saturate(dot(n, -forward));
    const float3 r = reflect(forward, n), bent = refract(forward, n, 0.67);
    const float3 cell = floor(n * 7);
    const float own = Hash(cell.xy + cell.z * 17.0);
    const float streak = pow(saturate(0.5 + 0.5 * sin(dot(bent, float3(9.0, 13.0, 6.0)))), 6);
    float3 c = float3(0.09, 0.14, 0.18) + rgb * 0.22 + float3(0.85, 0.92, 1.0) * streak * 0.5;
    c += min(ParlourMirrored(r), 1.2) * (0.08 + 0.4 * pow(1 - facing, 3));
    const float3 colours = 0.5 + 0.5 * cos(6.2831853 * (dot(r, float3(2.0, 3.0, 1.0)) + float3(0.0, 0.33, 0.67)));
    c += lerp(colours, float3(1, 1, 1), 0.55) * step(0.72, own) * (0.3 + 1.2 * (own - 0.72) / 0.28);
    return c;
}

// The golf course (look 6 with its kind, plus 10, in minus uv.z): 1 the ground, its surfaces blended by the
// vertices' weights (fairway, green, sand in local; water, tee in uv.xy; rough the rest), with
// the base colour already blended per vertex: grass with mown stripes, fine green, speckled
// sand, water with ripples and the sky's shine; 2 bark; 3 leaves in clumps; 4 plain (the flag,
// the pin). Distances are in metres (12.5 units a metre).
float3 CourseGround(float3 rgb, float3 local, float2 extra, float3 world, inout float3 glow)
{
    const float2 m = world.xz / 12.5;
    const float fairway = saturate(local.x), green = saturate(local.y), sand = saturate(local.z);
    const float water = saturate(extra.x), tee = saturate(extra.y);
    const float rough = saturate(1 - fairway - green - sand - water - tee);
    // Grass: clumps and blades, coarser in the rough.
    float grass = 0.93 + 0.14 * Noise(m * 0.45);
    grass *= lerp(1.0, 0.91 + 0.18 * Rough(m * 1.7), rough);
    grass *= lerp(1.0, 0.97 + 0.06 * Noise(m * 3.1), fairway + tee);
    // Mown stripes across the fairway and checks on the green.
    const float stripe = step(0.5, frac((m.y + 0.35 * m.x) / 7.0));
    grass *= lerp(1.0, lerp(0.96, 1.04, stripe), fairway);
    // (Faint, so the green's rise and fall shows in how the squares lean.)
    const float check = step(0.5, frac(m.x / 3.0)) == step(0.5, frac(m.y / 3.0)) ? 1.015 : 0.985;
    grass *= lerp(1.0, check, green);
    float3 c = rgb * lerp(1.0, grass, 1 - sand - water);
    // Sand: fine grains, raked lines.
    const float grain = 0.95 + 0.1 * Hash(floor(world.xz * 3.0)) + 0.03 * sin(m.x * 9.0 + 2.0 * Noise(m * 0.8));
    c = lerp(c, rgb * grain, sand);
    if (water > 0.01)
    {
        // Small waves and the sky over them.
        const float wave = Noise(m * 1.3 + 0.7 * sin(m.yx * 0.4)) * 0.6 + Noise(m * 3.7) * 0.4;
        const float3 deep = rgb * (0.75 + 0.35 * wave);
        glow += water * float3(0.55, 0.65, 0.75) * 0.35 * smoothstep(0.55, 0.9, wave);
        c = lerp(c, deep, water);
    }
    return c;
}

// Where the camera is (from the view matrix: the eye is where the view's origin comes from).
float3 CourseEye()
{
    const float3 t = View[3].xyz;
    return -float3(dot(t, View[0].xyz), dot(t, View[1].xyz), dot(t, View[2].xyz));
}

// The pond's surface height (metres) at m (metres): waves driven by the wind (Water.zw, world
// units a second), small ones always, and the rings spreading where something fell in (Ripples:
// x, z in world units, seconds since, how strong).
float WaveHeight(float2 m, float t)
{
    const float2 wind = Water.zw / 12.5;
    const float speed = length(wind);
    const float2 dir = speed > 0.05 ? wind / speed : float2(0.6, 0.8);
    const float2 side = float2(-dir.y, dir.x);
    const float a = 0.003 + 0.010 * saturate(speed / 6);
    float h = a * sin(dot(m, dir) * 1.7 - t * 2.3);
    h += a * 0.6 * sin(dot(m, normalize(dir + 0.45 * side)) * 2.9 - t * 3.2 + 1.3);
    h += a * 0.45 * sin(dot(m, normalize(dir - 0.55 * side)) * 4.6 - t * 4.1 + 2.1);
    h += a * 0.8 * (Noise(m * 1.9 - dir * t * 0.9) - 0.5) + 0.002 * (Noise(m * 6.0 + t * 0.7) - 0.5);
    [unroll] for (int k = 0; k < 16; ++k)
    {
        const float4 r = Ripples[k];
        if (r.w <= 0)
            continue;
        const float d = distance(m, r.xy / 12.5), front = 0.25 + r.z * 1.1, x = d - front;
        h += r.w * 0.025 * exp(-x * x * 10) * sin(x * 22) * exp(-r.z * 0.9) / (1 + d);
    }
    return h;
}

// The pond: its own waves bend the sky it mirrors (more at a low angle), the sun glints on
// it, and the water's own dark colour shows through where it is seen from above.
float3 CourseWater(float3 deep, float3 world)
{
    const float2 m = world.xz / 12.5;
    const float t = Water.y, e = 0.04;
    const float h = WaveHeight(m, t), hx = WaveHeight(m + float2(e, 0), t), hz = WaveHeight(m + float2(0, e), t);
    const float3 n = normalize(float3(-(hx - h) / e, 1, -(hz - h) / e));
    const float3 v = normalize(CourseEye() - world);
    const float3 r = reflect(-v, n);
    const float3 sky = lerp(float3(0.74, 0.82, 0.90), float3(0.30, 0.50, 0.82), saturate(r.y * 1.8)) +
                       float3(0.25, 0.25, 0.25) * smoothstep(0.55, 0.85, Noise(r.xz / max(r.y, 0.08) * 1.5));
    const float fresnel = 0.03 + 0.97 * pow(1 - saturate(dot(n, v)), 5);
    const float3 sun = -normalize(LightDir);
    const float3 body = deep * (0.55 + 0.45 * saturate(dot(n, sun)));
    float3 c = lerp(body, sky, saturate(fresnel * 1.1 + 0.22));
    c += float3(1.0, 0.96, 0.88) * pow(saturate(dot(n, normalize(sun + v))), 260) * 2.2;
    return c;
}

float3 Course(int kind, float3 rgb, float3 local, float4 uv, float3 world, inout float3 glow)
{
    if (kind == 1)
        return CourseGround(rgb, local, uv.xy, world, glow);
    if (kind == 2)
        return rgb * (0.75 + 0.4 * Noise(float2(world.x + world.z, world.y * 0.15) * 0.8));
    if (kind == 3)
    {
        const float clump = Rough(world.xz * 0.06 + world.y * 0.08);
        return rgb * (0.65 + 0.6 * clump) * (0.9 + 0.2 * Noise(world.xy * 0.4 + world.z * 0.3));
    }
    return rgb;
}

float4 main(VSOutput i) : SV_Target
{
    // See-through (a mahjong tile fading in as it falls): pixels left out in a 4x4 ordered
    // pattern, more of them the clearer it is (there is no blending here).
    if (i.uv.w > 0)
    {
        const uint2 at = (uint2)i.position.xy % 4;
        const float bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
        clip((bayer[at.y * 4 + at.x] + 0.5) / 16 - i.uv.w);
    }
    const int pattern = (int)round(i.color.a * 4);
    const int theme = (int)round((i.color.a * 4 - pattern) / 0.08);
    // Look 6 is shared: the parlour (its kinds 1-5 in minus uv.z, gilt and cut glass 6 and 7)
    // and the golf course (its kinds 11 and on, less 10; or a tiled picture, 2 in uv.z).
    const bool course = pattern == 0 && theme == 6 && (i.uv.z < -10.5 || i.uv.z > 1.5);
    const int courseKind = i.uv.z > 1.5 ? 0 : (int)round(-i.uv.z) - 10;
    if (pattern == 0 && theme == 6 && i.uv.z < -5.5 && i.uv.z > -10.5)
        return float4(i.uv.z < -6.5 ? CutGlass(i.color.rgb, i.world, normalize(i.normal)) : Gilt(i.color.rgb, i.world, normalize(i.normal)), 1);
    float3 rgb = i.color.rgb, glow = 0;
    if (i.uv.z > 1.5)
    {
        // Tiled, and cut out where the picture is clear (leaves).
        const float4 picture = Picture.Sample(WrapSampler, i.uv.xy);
        clip(picture.a - 0.5);
        rgb *= picture.rgb;
    }
    else if (i.uv.z > 0)
        rgb *= Picture.Sample(ClampSampler, i.uv.xy).rgb;
    if (pattern == 1)
    {
        // A soft light (the glowing pass's beams and trails, 9 in minus uv.z): bright along its
        // middle (uv.x 0.5), fading to nothing at both edges.
        if (i.uv.z < -8.5)
        {
            const float across = saturate(1 - abs(2 * i.uv.x - 1));
            rgb *= across * across;
        }
        return float4(rgb, 1);
    }
    float shine = 0;
    if (pattern == 0 && (theme == 1 || theme == 5))
    {
        // Wood: a grain running along the boards, the boards' dark seams, a little grime (look 5,
        // the mahjong room's: the same, its unevenness about half as strong).
        const float3 q = i.local;
        const float uneven = theme == 5 ? 0.5 : 1.0;
        rgb *= 0.95 - 0.17 * uneven + 0.34 * uneven * Noise(float2(q.x * 0.22 + q.z * 0.05, (q.y + q.z) * 3.2));
        const float board = frac((q.y + 0.37 * q.z) / 1.15);
        rgb *= lerp(0.55, 1, smoothstep(0.0, 0.07, min(board, 1 - board)));
        rgb *= 1 - 0.3 * uneven * smoothstep(0.55, 0.85, Noise(q.xy * 0.5 + q.z * 0.3 + 2.0));
    }
    else if (pattern == 0 && theme == 2)
    {
        // Metal: a little uneven, scratched, and shining where the light glances off it.
        const float3 q = i.local;
        rgb *= 0.85 + 0.25 * Noise(q.xy * 1.8 + q.z * 1.3);
        rgb *= 1 - 0.25 * Crack(float2(q.x + q.z, q.y) * 1.4, 0.015);
        shine = 0.55;
    }
    else if (pattern == 0 && theme == 4)
    {
        // Wallpaper: washi with its fibres, the waves pressed faintly into it.
        const float3 q = i.local;
        rgb *= 0.94 + 0.08 * Noise(q.xy * 9.0) + 0.04 * Noise(float2(q.x * 30.0, q.y * 3.0));
        rgb *= 1 - 0.10 * Seigaiha(q.xy);
    }
    else if (course)
        rgb = Course(courseKind, rgb, i.local, i.uv, i.world, glow);
    else if (pattern == 0 && theme == 6)
        rgb = Parlour((int)round(-i.uv.z), rgb, i.local);
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
    {
        rgb = Wall(theme, rgb, abs(i.normal.x) > 0.5 ? i.world.z : i.world.x, i.world.y, glow);
        // Wet and dark at their feet, above the water.
        if (Water.x > 0)
            rgb *= lerp(0.55, 1.0, smoothstep(0.2, 1.8, i.world.y));
    }
    const float lit = saturate(dot(normalize(i.normal), -normalize(LightDir)));
    float3 light = 0.55 + 0.6 * lit;
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
    // Fire and magic nearby.
    light += PointLights(i.world, normalize(i.normal), false);
    if (pattern == 3 && Water.x > 0 && i.normal.y > 0.5)
        return float4(Flooded(rgb, light, glow, i.world), 1);
    if (course && courseKind == 5)
        return float4(CourseWater(i.color.rgb, i.world), 1);
    if (course)
    {
        // The course fades into the haze far off (in metres from the camera).
        const float far_ = mul(float4(i.world, 1), View).z / 12.5;
        // (Textured scenery, its own far hills already pale, fades less.)
        const float haze = i.uv.z > 1.5 ? 0.45 : 0.75;
        return float4(lerp(rgb * light + glow, float3(0.72, 0.80, 0.88), haze * (1 - exp(-max(far_ - 60, 0) / 420))), 1);
    }
    return float4(rgb * light + glow, 1);
}
