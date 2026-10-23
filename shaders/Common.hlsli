// Shared declarations for the mascot PMX shaders (D3D12, SM 5.1).

#include "Scene.hlsli"

cbuffer Material : register(b1)
{
    float4 Diffuse;
    float3 Specular;
    float Shininess;
    float3 Ambient;
    float EdgeSize;
    float4 EdgeColor;
    float4 TextureMul;
    float4 TextureAdd;
    float4 SphereMul;
    float4 SphereAdd;
    float4 ToonMul;
    float4 ToonAdd;
    uint UseTexture;
    uint SphereMode;        // 0 none, 1 multiply, 2 add
    uint UseToon;
    uint ReceiveShadow;     // PMX "self shadow" flag
};

StructuredBuffer<float4x4> Bones : register(t0);
Texture2D ColorTexture : register(t1);
Texture2D SphereTexture : register(t2);
Texture2D ToonTexture : register(t3);
Texture2D<float> ShadowMap : register(t4);
SamplerState WrapSampler : register(s0);
SamplerState ClampSampler : register(s1);
SamplerComparisonState ShadowSampler : register(s2);

struct VSInput
{
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
    float3 normal : NORMAL;
    int4 bone : BLENDINDICES;
    float4 weight : BLENDWEIGHT;
    float3 sdefC : TEXCOORD1;
    uint weightType : TEXCOORD2;
    float edge : TEXCOORD3;
};

float4x4 BoneOrIdentity(int index)
{
    return index >= 0 ? Bones[index] : float4x4(1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1);
}

float4 QuatFromMatrix(float3x3 m)
{
    // m is row-vector convention (v' = v * m).
    float trace = m[0][0] + m[1][1] + m[2][2];
    float4 q;
    if (trace > 0)
    {
        float s = sqrt(trace + 1.0) * 2;
        q = float4((m[1][2] - m[2][1]) / s, (m[2][0] - m[0][2]) / s, (m[0][1] - m[1][0]) / s, 0.25 * s);
    }
    else if (m[0][0] > m[1][1] && m[0][0] > m[2][2])
    {
        float s = sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2;
        q = float4(0.25 * s, (m[1][0] + m[0][1]) / s, (m[2][0] + m[0][2]) / s, (m[1][2] - m[2][1]) / s);
    }
    else if (m[1][1] > m[2][2])
    {
        float s = sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2;
        q = float4((m[1][0] + m[0][1]) / s, 0.25 * s, (m[2][1] + m[1][2]) / s, (m[2][0] - m[0][2]) / s);
    }
    else
    {
        float s = sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2;
        q = float4((m[2][0] + m[0][2]) / s, (m[2][1] + m[1][2]) / s, 0.25 * s, (m[0][1] - m[1][0]) / s);
    }
    return normalize(q);
}

float3x3 MatrixFromQuat(float4 q)
{
    float x = q.x, y = q.y, z = q.z, w = q.w;
    return float3x3(
        1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),
        2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
        2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y));
}

float4 Slerp(float4 a, float4 b, float t)
{
    float d = dot(a, b);
    if (d < 0) { b = -b; d = -d; }
    if (d > 0.9995) return normalize(lerp(a, b, t));
    float theta = acos(d);
    return (a * sin((1 - t) * theta) + b * sin(t * theta)) / sin(theta);
}

void Skin(VSInput input, out float3 position, out float3 normal)
{
    if (input.weightType == 3)
    {
        // SDEF, same formulation as MikuMikuDayo skinning.hlsli.
        float3 c = input.sdefC;
        float w = input.weight.x;
        float3 hdr = input.weight.yzw;
        float3 cr1 = c - w * hdr;
        float3 cr0 = cr1 + hdr;
        float4x4 m0 = BoneOrIdentity(input.bone.x);
        float4x4 m1 = BoneOrIdentity(input.bone.y);
        float3x3 r = MatrixFromQuat(Slerp(QuatFromMatrix((float3x3)m1), QuatFromMatrix((float3x3)m0), w));
        position = mul(input.position - c, r) + lerp(mul(float4(cr1, 1), m1).xyz, mul(float4(cr0, 1), m0).xyz, w);
        normal = mul(input.normal, r);
        return;
    }
    float4x4 m = 0;
    [unroll] for (int i = 0; i < 4; ++i)
    {
        if (input.bone[i] >= 0) m += Bones[input.bone[i]] * input.weight[i];
    }
    position = mul(float4(input.position, 1), m).xyz;
    normal = mul(input.normal, (float3x3)m);
}
