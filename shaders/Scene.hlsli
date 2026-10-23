// Scene constants shared by the model, edge and particle shaders.

cbuffer Scene : register(b0)
{
    float4x4 ViewProj;      // model: its display transform (warp spin and shrink) included
    float4x4 View;
    float3 LightDir;        // direction the light travels (MMD default: -0.5,-1,0.5)
    float EdgePixels;       // outline width in pixels for edge size 1
    float3 LightColor;
    float ScenePad0;
    float2 ViewportSize;
    float2 ScenePad1;
    float4x4 ParticleViewProj;   // plain camera, for effects drawn in world space
    float4 CameraRight;
    float4 CameraUp;
    float4x4 LightViewProj; // model space to the self-shadow map
    float4 ShadowInfo;      // x: self-shadow on, y: one shadow-map texel in UV
};
