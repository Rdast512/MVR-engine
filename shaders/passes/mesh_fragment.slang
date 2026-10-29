#include "types.slang"
#include "core/frame.slang"
#include "material/pbr.slang"

[[vk::push_constant]] ConstantBuffer<MeshPushData> push;

[shader("fragment")]
float4 fragMain(MeshVertexOut vertIn) : SV_TARGET
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    return sampleBaseColor(loadMaterial(root, vertIn.materialRow), vertIn.fragTexCoord, vertIn.fragColor);
}
