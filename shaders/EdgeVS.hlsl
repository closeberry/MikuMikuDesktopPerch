#include "Common.hlsli"

struct VSOutput
{
    float4 position : SV_Position;
    float4 clipNow : TEXCOORD4;
    float4 clipPrev : TEXCOORD5;
};

VSOutput main(VSInput input)
{
    float3 position, normal;
    Skin(input, position, normal);
    float4 clipPosition = mul(float4(position, 1), ViewProj);
    // Screen-constant outline: EdgePixels * edge size * per-vertex edge scale, pushed out along
    // the normal's sideways (view-space xy) part. Not normalized: a normal facing the camera
    // gets almost no push, where a normalized direction would point anywhere and fold thin
    // outline triangles over the face (lines along the mesh edges); at the silhouette the
    // normal is all sideways and the outline is full width.
    float3 viewNormal = normalize(mul(float4(normal, 0), View).xyz);
    float2 offset = viewNormal.xy * EdgeSize * input.edge * EdgePixels * 2.0 / ViewportSize;
    VSOutput output;
    output.clipPrev = PreviousClip(input, clipPosition);
    output.clipPrev.xy += offset * output.clipPrev.w;
    clipPosition.xy += offset * clipPosition.w;
    // A little behind the surface, so the outline never wins the depth test at creases.
    clipPosition.z += 0.0005 * clipPosition.w;
    output.position = clipPosition;
    output.clipNow = clipPosition;
    return output;
}
