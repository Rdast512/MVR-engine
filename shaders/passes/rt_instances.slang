import types;
import core.binding;

// one TLAS instance per ray instance (spec/features/ray_tracing_mega_geometry.md §3.2): the transform comes from this
// frame slot's node instance, the BLAS from the layout's address table. Runs on the async compute queue before the
// TLAS build

static const uint kRtInstancesGroupSize = 64; // matches src/render/vk_rt_scene.cpp
static const uint kInstanceMaskAll = 0xFFu;
// VK_GEOMETRY_INSTANCE_FORCE_OPAQUE_BIT_KHR: no any-hit; alpha-tested primitives switch to OMM 2-state in plan D
static const uint kInstanceForceOpaque = 0x4u;

// VkAccelerationStructureInstanceKHR, 64 B
struct TlasInstance
{
    float4 transform[3];     // rows of the 3x4 object-to-world matrix
    uint customIndexAndMask; // instanceCustomIndex:24 | mask:8
    uint sbtOffsetAndFlags;  // instanceShaderBindingTableRecordOffset:24 | flags:8
    uint64_t blas;
};

[[vk::push_constant]] ConstantBuffer<RtInstancesPushData> push;

[shader("compute")]
[numthreads(kRtInstancesGroupSize, 1, 1)]
void rtInstancesMain(uint3 dispatchId: SV_DispatchThreadID)
{
    const uint row = dispatchId.x;
    if (row >= push.count)
    {
        return;
    }
    GpuFrameRoot* root = bdaPtr<GpuFrameRoot>(push.frameRoot);
    const GpuRayInstance ray = *(bdaPtr<GpuRayInstance>(root.rayInstances) + row);
    GpuNodeInstance* node = bdaPtr<GpuNodeInstance>(root.nodeInstances) + ray.nodeInstanceIndex;
    const float4x4 world = node.worldMatrix;

    TlasInstance instance;
    // world[r] is row r with the translation in w, the layout VkTransformMatrixKHR expects
    instance.transform[0] = world[0];
    instance.transform[1] = world[1];
    instance.transform[2] = world[2];
    // InstanceID() = primitive row: the SER hint skips the GpuRayInstance load (§4.3); the host checks the 24 bits
    instance.customIndexAndMask = ray.primitiveRow | (kInstanceMaskAll << 24);
    // one hit group: record offset 0. No facing flags: RT handles mirrored transforms itself
    instance.sbtOffsetAndFlags = kInstanceForceOpaque << 24;
    instance.blas = *(bdaPtr<uint64_t>(push.blasAddresses) + row);
    *(bdaPtr<TlasInstance>(push.tlasInstances) + row) = instance;
}
