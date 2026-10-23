#include "Common.hlsli"

struct PSInput
{
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float3 viewNormal : TEXCOORD1;
    float3 worldPosition : TEXCOORD2;
    float2 uv : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target
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
    if (UseToon)
    {
        float lambert = dot(n, toLight);
        float4 toon = ToonTexture.Sample(ClampSampler, float2(0, 0.5 - lambert * 0.5)) * ToonMul + ToonAdd;
        color.rgb *= toon.rgb;
    }
    if (Shininess > 0)
    {
        float3 eyeDir = normalize(float3(View._13, View._23, View._33));
        float3 h = normalize(toLight - eyeDir);
        color.rgb += Specular * LightColor * pow(max(0, dot(h, n)), Shininess);
    }
    color = saturate(color);
    clip(color.a - 1.0 / 255.0);
    return color;
}
