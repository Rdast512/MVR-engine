#include "types.slang"
#include "material/pbr.slang"

[[vk::push_constant]] ConstantBuffer<MeshPushData> push;

[shader("fragment")]
float4 fragMain(MeshVertexOut vertIn) : SV_TARGET
{
    return sampleAlbedo(push, vertIn.fragTexCoord);
}
