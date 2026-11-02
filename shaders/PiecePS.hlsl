// The chess men. The colour's alpha says what a vertex is:
// below 1: a soft pool on the board (blended; see SetPieces): of shadow, or of the light under a
//    crystal man, its colour tint.rgb, tint.a how strong;
// 1: plain (the knight's eyes, the felt under the metal men), lit by the scene;
// 2: cut crystal, drawn after everything behind it (premultiplied): its back faces, then its
//    front. Dark and clear inside, bright at its outline, a lattice engraved on its stem, ridges
//    on its base, which glows as if lit from below. tint.a: the man's height in its own units.
// 3: metal: the room mirrored in it, the colour being what it mirrors of each light.
// 4: an aura, added to what is there, turning with its own place in the world: red 0, a sheath
//    of light winding up from the feet; red 1, a ring with marks on the ground. Its colour is
//    tint.rgb, tint.a how strong.
// Tint: rgb glows.
#include "Scene.hlsli"

struct VSOutput
{
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
    float3 local : TEXCOORD1;
    float3 normal : NORMAL;
    float4 color : COLOR0;
    float4 tint : COLOR1;
};

// The room as a mirror sees it in a direction: the table and board below, the warm walls, the
// paper screens around, and the lamp above.
float3 Room(float3 r)
{
    float3 c = r.y < 0 ? lerp(float3(0.34, 0.25, 0.16), float3(0.05, 0.17, 0.11), saturate(-r.y * 2.2))
                       : lerp(float3(0.26, 0.20, 0.15), float3(0.07, 0.055, 0.05), saturate(r.y * 1.3));
    const float around = atan2(r.x, r.z);
    const float screens = smoothstep(0.45, 0.72, cos(around * 3)) * smoothstep(0.62, 0.22, abs(r.y - 0.2));
    c += float3(1.7, 1.52, 1.2) * screens * (0.8 + 0.2 * step(0.5, frac(around * 7)));
    c += float3(1.6, 1.4, 1.0) * pow(saturate(dot(r, float3(0, 0.995, 0.1))), 160);
    return c;
}

float4 main(VSOutput i, bool front : SV_IsFrontFace) : SV_Target
{
    if (i.color.a < 0.999)
        return float4(i.tint.rgb, i.color.a * i.tint.a);
    if (i.color.a > 3.5)
    {
        const float a = atan2(i.local.x, i.local.z);
        float v;
        if (i.color.r > 0.5)
        {
            const float r = length(i.local.xz);
            const float band = smoothstep(0.70, 0.74, r) * (1 - smoothstep(0.90, 0.94, r));
            const float marks = pow(saturate(0.5 + 0.5 * cos(a * 12)), 6) * band;
            const float d0 = (r - 1.0) / 0.035, d1 = (r - 0.66) / 0.022;
            v = exp(-d0 * d0) * 1.25 + exp(-d1 * d1) * 0.8 + marks * 0.95 + 0.13 * band + 0.22 * exp(-4 * r * r);
            v += 0.5 * pow(saturate(0.5 + 0.5 * cos(a * 3)), 12) * exp(-((r - 1.0) * (r - 1.0)) / 0.02);
        }
        else
        {
            const float y = i.local.y;
            const float wind = pow(saturate(0.5 + 0.5 * cos(a * 4 + y * 8)), 2.5);
            const float fine = pow(saturate(0.5 + 0.5 * cos(a * 9 - y * 13)), 4);
            v = (wind * 0.62 + fine * 0.30 + 0.12) * pow(saturate(1 - y), 1.5);
        }
        return float4(i.tint.rgb * v * i.tint.a, 0);
    }
    const float3 forward = normalize(cross(CameraRight.xyz, CameraUp.xyz));
    const float3 l = -normalize(LightDir);
    const float3 half_ = normalize(l - forward);
    float3 n = normalize(i.normal);
    if (i.color.a > 2.5)
    {
        // Metal: nearly all mirror.
        const float facing = saturate(dot(n, -forward));
        const float3 mirror = i.color.rgb + (1 - i.color.rgb) * pow(1 - facing, 5);
        float3 rgb = (Room(reflect(forward, n)) + 0.08) * mirror;
        rgb += i.color.rgb * 0.10 * (0.5 + saturate(dot(n, l)));
        rgb += float3(1.0, 0.97, 0.92) * (pow(saturate(dot(n, half_)), 120) * 1.1 + pow(saturate(dot(n, half_)), 18) * 0.10);
        return float4(rgb + i.tint.rgb, 1);
    }
    if (i.color.a > 1.5)
    {
        float3 flat_ = normalize(cross(ddx(i.world), ddy(i.world)));
        if (dot(flat_, n) < 0)
            flat_ = -flat_;
        const float3 smooth_ = n;
        n = normalize(lerp(n, flat_, 0.55));
        if (!front)
            n = -n;
        const float facing = saturate(dot(n, -forward));
        const float glance = pow(1 - facing, 2.0);
        const float height = i.local.y / max(i.tint.a, 0.001);
        // A lattice engraved round the man: two sets of lines winding opposite ways; ridges below.
        const float around = atan2(i.local.x, i.local.z) / 6.2831853 * 8;
        const float rise = i.local.y * 0.72;
        const float a1 = abs(frac(around + rise) - 0.5), a2 = abs(frac(around - rise) - 0.5);
        const float lines = smoothstep(0.42, 0.485, max(a1, a2));
        const float stem = smoothstep(0.20, 0.27, height);
        const float ridge = 0.5 + 0.5 * cos(i.local.y * 15.0);
        const float ridges = (1 - stem) * pow(ridge, 3);
        // Inside: dark, clear blue, with the bright things round it bent into streaks.
        const float3 bent = refract(forward, n, 0.67);
        const float streak = pow(saturate(0.5 + 0.5 * sin(dot(bent, float3(7.0, 11.0, 5.0)) + height * 9.0)), 5);
        float3 rgb = float3(0.035, 0.06, 0.13) + float3(0.62, 0.74, 1.0) * streak * 0.62;
        // Lit from below, fading up the man.
        const float below = pow(saturate(1 - height / 0.5), 2);
        rgb += float3(0.34, 0.50, 1.0) * below * (0.10 + 0.62 * ridge);
        // A warm light from the left, as from a lamp across the room.
        rgb += float3(1.0, 0.68, 0.28) * pow(saturate(dot(smooth_, normalize(float3(-0.85, 0.15, -0.5)))), 2.5) * 0.42;
        // The outline, bright; the engraving and the ridges catching the light.
        rgb += float3(0.82, 0.92, 1.0) * glance * (0.45 + 0.65 * stem);
        rgb += float3(0.86, 0.93, 1.0) * (lines * stem * (0.60 - 0.25 * facing) + ridges * 0.16);
        rgb += min(Room(reflect(forward, n)), 1.0) * (0.03 + (0.15 + 0.3 * stem) * pow(1 - facing, 3));
        // Between the base's ridges it stays dark, so that they show.
        rgb *= lerp(0.45 + 0.55 * ridge, 1.0, stem);
        const float glint = pow(saturate(dot(n, half_)), 110);
        rgb += float3(1.0, 1.0, 1.0) * glint * lerp(1.1, 2.6, stem);
        if (!front)
            return float4(rgb * 0.22 + i.tint.rgb * 0.2, 0.10);
        const float alpha = saturate(lerp(0.70, 0.98, glance) + lines * stem * 0.25 + glint);
        return float4(rgb + i.tint.rgb, alpha);
    }
    n = front ? n : -n;
    const float3 ambient = lerp(float3(0.20, 0.16, 0.12), float3(0.52, 0.48, 0.44), n.y * 0.5 + 0.5);
    const float3 rgb = i.color.rgb * (ambient + 0.72 * saturate(dot(n, l)) + PointLights(i.world, n, false));
    return float4(rgb + i.tint.rgb, 1);
}
