#include "types.slang"
#include "core/frame.slang"
#include "material/pbr.slang"

[[vk::push_constant]] ConstantBuffer<MeshPushData> push;

[shader("fragment")]
float4 fragMain(MeshVertexOut vertIn) : SV_TARGET
{
    DrawContext draw = loadDrawContext(push);
    return sampleBaseColor(loadMaterial(draw), vertIn.fragTexCoord, vertIn.fragColor);
}
