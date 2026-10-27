// Camera-facing sparkle quads, one instance per particle.
#include "Scene.hlsli"

struct Particle
{
    float3 position;
    float size;         // half the quad's side, world units
    float4 color;       // straight alpha
    float angle;        // spin of the star shape
    float glow;         // 1: a soft halo (ParticlePS)
    float2 pad;
};
StructuredBuffer<Particle> Particles : register(t0);

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    float glow : TEXCOORD1;
};

VSOutput main(uint vertex : SV_VertexID, uint instance : SV_InstanceID)
{
    static const float2 corners[6] = { float2(-1,-1), float2(-1,1), float2(1,1), float2(-1,-1), float2(1,1), float2(1,-1) };
    const Particle p = Particles[instance];
    const float2 c = corners[vertex];
    float s, co;
    sincos(p.angle, s, co);
    const float2 r = float2(c.x * co - c.y * s, c.x * s + c.y * co);
    float3 world = p.position + (CameraRight.xyz * r.x + CameraUp.xyz * r.y) * p.size;
    // A halo is brought toward the camera by most of its size, so the floor and the thing
    // glowing do not cut it, while a wall between still hides it.
    if (p.glow > 0)
        world -= normalize(cross(CameraRight.xyz, CameraUp.xyz)) * p.size * 0.8;
    VSOutput o;
    o.position = mul(float4(world, 1), ParticleViewProj);
    o.uv = c;
    o.color = p.color;
    o.glow = p.glow;
    return o;
}
