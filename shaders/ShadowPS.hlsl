#include "Common.hlsli"

// Cut-out parts of a texture (hair strands, lace) cast no shadow.
void main(float4 position : SV_Position, float2 uv : TEXCOORD0)
{
    float alpha = Diffuse.a;
    if (UseTexture) alpha *= ColorTexture.Sample(WrapSampler, uv).a * TextureMul.a + TextureAdd.a;
    clip(alpha - 0.5);
}
