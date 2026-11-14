import types;
import core.binding;
import core.frame;

// DLSS-RR output (Lit) or the debug display image -> swapchain, scaled when render != display; ImGui draws after it
// in the same rendering.
// M3's composite keeps this stage: the sRGB swapchain has no storage support

[[vk::push_constant]] ConstantBuffer<PresentPushData> push;

struct PresentVertexOut
{
    float4 pos : SV_Position;
};

[shader("mesh")]
[numthreads(1, 1, 1)]
[outputtopology("triangle")]
void presentMesh(out vertices PresentVertexOut verts[3], out indices uint3 tris[1])
{
    SetMeshOutputCounts(3, 1);
    verts[0].pos = float4(-1.0, -1.0, 0.0, 1.0);
    verts[1].pos = float4(3.0, -1.0, 0.0, 1.0);
    verts[2].pos = float4(-1.0, 3.0, 0.0, 1.0);
    tris[0] = uint3(0, 1, 2);
}

[shader("fragment")]
float4 presentFrag(PresentVertexOut vertIn) : SV_TARGET
{
    const GpuTargets targets = loadTargets(bdaPtr<GpuFrameRoot>(push.frameRoot)[0]);
    const float2 uv = vertIn.pos.xy / float2(push.displaySize);
    return tex2D(targets.presentSampled).SampleLevel(samplerOf(targets.linearSampler), uv, 0.0);
}
