#include "Common.hlsli"

struct PSInput
{
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float3 viewNormal : TEXCOORD1;
    float3 worldPosition : TEXCOORD2;
    float2 uv : TEXCOORD0;
    float4 shadowPosition : TEXCOORD3;
    float4 clipNow : TEXCOORD4;
    float4 clipPrev : TEXCOORD5;
};

struct PSOutput
{
    float4 color : SV_Target0;
    float2 motion : SV_Target1;   // for motion blur (no target bound when it is off)
};

// 1 where the light reaches, 0 in the model's own shadow, softened over a few texels.
float SelfShadow(float4 shadowPosition)
{
    float3 p = shadowPosition.xyz / shadowPosition.w;
    float2 uv = p.xy * float2(0.5, -0.5) + 0.5;
    float lit = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
            lit += ShadowMap.SampleCmpLevelZero(ShadowSampler, uv + float2(x, y) * ShadowInfo.y, p.z);
    return p.z > 1 ? 1 : lit / 9;   // beyond the map's far end: lit (the border is lit too)
}

PSOutput main(PSInput input)
{
    // MMD standard shading: ambient + diffuse*light, then texture, sphere, toon and specular.
    float3 n = normalize(input.normal);
    float3 toLight = -normalize(LightDir);
    float4 color = float4(saturate(Ambient + Diffuse.rgb * LightColor), Diffuse.a);

    if (UseTexture)
    {
        color *= ColorTexture.Sample(WrapSampler, input.uv) * TextureMul + TextureAdd;
    }
    if (SphereMode == 1 || SphereMode == 2)
    {
        float3 vn = normalize(input.viewNormal);
        float2 sphereUv = vn.xy * float2(0.5, -0.5) + 0.5;
        float4 sphere = SphereTexture.Sample(WrapSampler, sphereUv) * SphereMul + SphereAdd;
        if (SphereMode == 1) color.rgb *= sphere.rgb;
        else color.rgb += sphere.rgb;
        color.a *= sphere.a;
    }
    float lambert = dot(n, toLight);
    float lit = 1;
    if (ShadowInfo.x > 0 && ReceiveShadow)
    {
        // MMD's self-shadow mode: shadowed parts (cast by the model, or turned away from the
        // light, with a sharper edge than the plain toon ramp) take the toon's dark end, lit
        // parts no toon at all. Without a toon both are the same colour, as in MMD, so only
        // the highlight goes.
        lit = SelfShadow(input.shadowPosition);
        if (UseToon)
        {
            float3 dark = (ToonTexture.Sample(ClampSampler, float2(0, 1)) * ToonMul + ToonAdd).rgb;
            lit = min(saturate(lambert * 3), lit);
            color.rgb *= lerp(dark, 1, lit);
        }
    }
    else if (UseToon)
    {
        float4 toon = ToonTexture.Sample(ClampSampler, float2(0, 0.5 - lambert * 0.5)) * ToonMul + ToonAdd;
        color.rgb *= toon.rgb;
    }
    if (Shininess > 0)
    {
        float3 eyeDir = normalize(float3(View._13, View._23, View._33));
        float3 h = normalize(toLight - eyeDir);
        color.rgb += Specular * LightColor * pow(max(0, dot(h, n)), Shininess) * lit;
    }
    // Fire and magic nearby tint her.
    color.rgb *= 1 + PointLights(input.worldPosition, n, true);
    color = saturate(color);
    clip(color.a - 1.0 / 255.0);
    PSOutput output;
    output.color = color;
    output.motion = ScreenMotion(input.clipNow, input.clipPrev);
    return output;
}
