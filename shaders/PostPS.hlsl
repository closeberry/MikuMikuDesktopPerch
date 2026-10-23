// Motion blur: each pixel gathers the frame along the strongest motion around it, so a moving
// limb smears over what lies behind it and turns see-through at its trailing edges, while
// what holds still stays sharp. (After McGuire et al., "A Reconstruction Filter for Plausible
// Motion Blur", simplified: a ring search instead of tiles, no depth.)
#include "Scene.hlsli"

Texture2D<float4> SceneColor : register(t1);   // premultiplied
Texture2D<float2> SceneMotion : register(t2);  // pixels since last frame

int2 Inside(int2 p) { return clamp(p, int2(0, 0), int2(ViewportSize) - 1); }

// The smear this pixel's motion makes: the shutter's share of it, no longer than the limit.
float2 Smear(int2 p)
{
    const float2 v = SceneMotion.Load(int3(Inside(p), 0)) * BlurInfo.x;
    const float l = length(v);
    return l > BlurInfo.y ? v * (BlurInfo.y / l) : v;
}

float4 main(float4 position : SV_Position) : SV_Target
{
    const int2 p = int2(position.xy);
    const float4 center = SceneColor.Load(int3(p, 0));
    const float2 own = Smear(p);

    // The strongest smear that can reach here (from at most half the longest smear away).
    float2 strongest = own;
    float best = dot(own, own);
    static const float2 around[8] = { float2(1, 0), float2(0.7071, 0.7071), float2(0, 1), float2(-0.7071, 0.7071),
                                      float2(-1, 0), float2(-0.7071, -0.7071), float2(0, -1), float2(0.7071, -0.7071) };
    [unroll] for (int ring = 1; ring <= 3; ++ring)
        [unroll] for (int k = 0; k < 8; ++k)
        {
            const float2 v = Smear(p + int2(round(around[k] * BlurInfo.y * ring / 6.0)));
            const float d = dot(v, v);
            if (d > best) { best = d; strongest = v; }
        }
    if (best < 0.25) return center;

    // Samples along it, offset per pixel against banding. One counts where the motion of
    // either end covers the distance between them; the rest stay as this pixel is.
    const float noise = frac(52.9829189 * frac(dot(position.xy, float2(0.06711056, 0.00583715))));
    const float ownReach = 0.5 * length(own) + 0.5;
    const int count = 12;
    float4 sum = 0;
    float taken = 0;
    [unroll] for (int i = 0; i < count; ++i)
    {
        const float t = (i + noise) / count - 0.5;
        const float2 offset = strongest * t;
        const int2 q = Inside(p + int2(round(offset)));
        const float distance = length(offset);
        const float reach = 0.5 * length(Smear(q)) + 0.5;
        if (distance <= reach || distance <= ownReach)
        {
            sum += SceneColor.Load(int3(q, 0));
            taken += 1;
        }
    }
    return (sum + center * (count - taken)) / count;
}
