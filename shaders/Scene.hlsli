// Scene constants shared by the model, edge and particle shaders.

cbuffer Scene : register(b0)
{
    float4x4 ViewProj;      // model: its display transform (warp spin and shrink) included
    float4x4 View;
    float3 LightDir;        // direction the light travels (MMD default: -0.5,-1,0.5)
    float EdgePixels;       // outline width in pixels for edge size 1
    float3 LightColor;
    float TorchRadius;      // the dungeon's torch around the hero (0: none), see GeometryPS
    float2 ViewportSize;
    float2 TorchXZ;
    float4x4 ParticleViewProj;   // plain camera, for effects drawn in world space
    float4 CameraRight;
    float4 CameraUp;
    float4x4 LightViewProj; // model space to the self-shadow map
    float4 ShadowInfo;      // x: self-shadow on, y: one shadow-map texel in UV
    // Motion blur: where things were drawn last frame.
    float4x4 PrevViewProj;  // last frame's ViewProj
    float4 MotionInfo;      // xy: how far the frame moved on screen since then (pixels), zw: its viewport size
    float4 BlurInfo;        // x: shutter (share of a frame's motion smeared), y: longest smear (pixels),
                            // zw: drawn pixels per window pixel (below 1 when the frame is scaled up)
    uint4 BlurFlags;        // x: motion blur on, y: index of last frame's bones in Bones
    // Point lights of fire and magic (MascotRenderer::SetLights): xyz and the radius they fade
    // out at (0: none), their colours, and their places in the model's own space.
    float4 LightPlaces[8];
    float4 LightColors[8];
    float4 ModelLightPlaces[8];
};

// The point lights' light at p (world, or the model's own space) on a surface facing n:
// fading with the square toward each one's radius, softly toward the side turned away.
float3 PointLights(float3 p, float3 n, bool inModel)
{
    float3 sum = 0;
    [unroll] for (int i = 0; i < 8; ++i)
    {
        const float4 l = inModel ? ModelLightPlaces[i] : LightPlaces[i];
        if (l.w <= 0)
            continue;
        const float3 d = l.xyz - p;
        const float distance = length(d);
        float f = saturate(1 - distance / l.w);
        f *= f;
        sum += LightColors[i].rgb * f * (0.35 + 0.65 * saturate(dot(n, d / max(distance, 1e-3))));
    }
    return sum;
}

// How far a point moved on screen since last frame, in pixels (from its clip positions now and then).
float2 ScreenMotion(float4 now, float4 prev)
{
    const float2 a = (now.xy / now.w * float2(0.5, -0.5) + 0.5) * ViewportSize;
    const float2 b = (prev.xy / prev.w * float2(0.5, -0.5) + 0.5) * MotionInfo.zw;
    return a - b + MotionInfo.xy;
}
