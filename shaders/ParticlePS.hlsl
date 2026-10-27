// A glowing white-hot core with four long rays and four fainter diagonal ones, or (glow) a
// soft round halo of light, added more than laid over.
struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float glow : TEXCOORD1;
};

float Ray(float along, float across)
{
    return max(0, 1 - abs(across) * 10) * pow(max(0, 1 - abs(along)), 2);
}

float4 main(VSOutput i) : SV_Target
{
    const float2 u = i.uv;
    if (i.glow > 0)
    {
        const float r2 = dot(u, u);
        const float h = (exp(-r2 * 3.5) - exp(-3.5)) / (1 - exp(-3.5)) * i.color.a;
        clip(h - 1.0 / 255.0);
        return float4(i.color.rgb * h, h * 0.2);
    }
    const float2 d = float2(u.x + u.y, u.x - u.y) * 0.7071;
    const float core = exp(-dot(u, u) * 14.0);
    const float halo = exp(-dot(u, u) * 3.0) * 0.25;
    const float rays = Ray(u.x, u.y) + Ray(u.y, u.x) + 0.45 * (Ray(d.x * 1.4, d.y) + Ray(d.y * 1.4, d.x));
    const float a = saturate(core + halo + rays) * i.color.a;
    clip(a - 1.0 / 255.0);
    // Toward white where the light is strongest; premultiplied, with coverage a little below
    // the colour so the sparks glow over the desktop.
    const float3 rgb = lerp(i.color.rgb, float3(1, 1, 1), saturate(core * 1.2 + rays * 0.3));
    return float4(rgb * a, a * 0.7);
}
