import types;
import core.binding;

// fullscreen view of one Hi-Z mip: one mesh workgroup emits a screen-covering triangle

[[vk::push_constant]] ConstantBuffer<HiZViewPushData> push;

struct HiZViewVertexOut
{
    float4 pos : SV_Position;
};

[shader("mesh")]
[numthreads(1, 1, 1)]
[outputtopology("triangle")]
void hizViewMesh(out vertices HiZViewVertexOut verts[3], out indices uint3 tris[1])
{
    SetMeshOutputCounts(3, 1);
    verts[0].pos = float4(-1.0, -1.0, 0.0, 1.0);
    verts[1].pos = float4(3.0, -1.0, 0.0, 1.0);
    verts[2].pos = float4(-1.0, 3.0, 0.0, 1.0);
    tris[0] = uint3(0, 1, 2);
}

// log-scaled linear depth: near white, far and empty black
[shader("fragment")]
float4 hizViewFrag(HiZViewVertexOut vertIn) : SV_TARGET
{
    const Texture2D pyramid = tex2D(push.pyramid);
    uint width;
    uint height;
    uint levels;
    pyramid.GetDimensions(0, width, height, levels);
    const uint mip = min(push.mip, levels - 1);
    const uint2 mipSize = max(uint2(width, height) >> mip, uint2(1, 1));
    const uint2 texel = min(uint2(vertIn.pos.xy * float2(mipSize) / float2(push.screenSize)), mipSize - 1);
    const float depth = pyramid.Load(int3(texel, mip)).r;

    // reverse Z: 1 at near, 0 at far
    const float linearZ = push.nearZ * push.farZ / (push.nearZ + depth * (push.farZ - push.nearZ));
    const float t = saturate(log(linearZ / push.nearZ) / log(push.farZ / push.nearZ));
    return float4((1.0 - t).xxx, 1.0);
}
