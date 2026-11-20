import types;
import core.binding;

// PTLAS instance writes (PBR plan C4, spec/features/ray_tracing_mega_geometry.md §3.6). One thread per ray instance.
// A full build writes every row; otherwise a row writes only when its transform differs from the one the PTLAS
// holds. Instances stay in their grid partition: the global partition measured slower on the 5070 (§3.6). Runs on
// the async compute queue before the PTLAS build

static const uint kRtInstancesGroupSize = 64; // matches src/render/vk_rt_scene.cpp
static const uint kInstanceMaskAll = 0xFFu;
// VK_PARTITIONED_ACCELERATION_STRUCTURE_INSTANCE_FLAG_FORCE_OPAQUE_BIT_NV: no any-hit. Primitives with a micromap drop
// it and carry no opacity flag; rays force OMM 2-state themselves (spec/m6/d2_micromap_clas.md)
static const uint kPtlasInstanceForceOpaque = 0x4u;

// VkPartitionedAccelerationStructureWriteInstanceDataNV, 104 B
struct PtlasWriteInstance
{
    float4 transform[3]; // rows of the 3x4 object-to-world matrix
    float explicitAabb[6];
    uint instanceId;     // InstanceID()
    uint instanceMask;
    uint hitGroupOffset;
    uint instanceFlags;
    uint instanceIndex;  // InstanceIndex(): the ray-instance row
    uint partitionIndex;
    uint64_t blas;
};

// VkBuildPartitionedAccelerationStructureIndirectCommandNV, 24 B
struct PtlasOperation
{
    uint opType;
    uint argCount;
    uint64_t argStart;
    uint64_t argStride;
};

// what the PTLAS holds for one instance, 48 B (kPtlasStateRowBytes in src/render/vk_rt_scene.cpp)
struct PtlasInstanceState
{
    float4 transform[3];
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
    PtlasInstanceState* state = bdaPtr<PtlasInstanceState>(push.ptlasState) + row;
    const GpuRayInstance ray = *(bdaPtr<GpuRayInstance>(root.rayInstances) + row);
    const float4x4 world = (bdaPtr<GpuNodeInstance>(root.nodeInstances) + ray.nodeInstanceIndex).worldMatrix;
    // world[r] is row r with the translation in w, the layout VkTransformMatrixKHR expects
    float4 rows[3] = { world[0], world[1], world[2] };

    bool isWritten = (push.flags & kRtInstancesFlagFullBuild) != 0;
    if (!isWritten)
    {
        const PtlasInstanceState held = *state;
        isWritten = any(held.transform[0] != rows[0]) || any(held.transform[1] != rows[1]) ||
                    any(held.transform[2] != rows[2]);
    }

    // one atomic per wave on the op's argument count
    const uint waveCount = WaveActiveCountBits(isWritten);
    uint base = 0;
    if (WaveIsFirstLane() && waveCount > 0)
    {
        PtlasOperation* operation = bdaPtr<PtlasOperation>(push.operation);
        InterlockedAdd(operation.argCount, waveCount, base);
    }
    base = WaveReadLaneFirst(base);
    if (!isWritten)
    {
        return;
    }

    const GpuRayInstanceBuild build = *(bdaPtr<GpuRayInstanceBuild>(push.buildRows) + row);
    state.transform = rows;
    PtlasWriteInstance instance;
    instance.transform = rows;
    instance.explicitAabb = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };
    // InstanceID() = primitive row: the SER hint skips the GpuRayInstance load (§4.3); the host checks the 24 bits
    instance.instanceId = ray.primitiveRow;
    instance.instanceMask = kInstanceMaskAll;
    // one hit group: record offset 0. No facing flags: RT handles mirrored transforms itself
    instance.hitGroupOffset = 0;
    instance.instanceFlags = (build.flags & kRayInstanceBuildFlagOpacityMicromap) != 0 ? 0u : kPtlasInstanceForceOpaque;
    instance.instanceIndex = row;
    instance.partitionIndex = build.partition;
    instance.blas = build.blas;
    *(bdaPtr<PtlasWriteInstance>(push.writeInfos) + base + WavePrefixCountBits(isWritten)) = instance;
}
