#include "types.slang"
#include "core/binding.slang"

// instances -> draw records -> per state group visible lists -> DGC sequences + counts

[[vk::push_constant]] ConstantBuffer<CullPushData> push;

// largest axis scale of the affine part: the world radius of a unit sphere
float maxAxisScale(float4x4 world)
{
    const float3 c0 = float3(world[0][0], world[1][0], world[2][0]);
    const float3 c1 = float3(world[0][1], world[1][1], world[2][1]);
    const float3 c2 = float3(world[0][2], world[1][2], world[2][2]);
    return sqrt(max(dot(c0, c0), max(dot(c1, c1), dot(c2, c2))));
}

// backfacing is affine invariant, so the cone test runs in object space and stays exact under non-uniform scale
float3 worldToObject(float4x4 world, float3 position)
{
    const float3 c0 = float3(world[0][0], world[1][0], world[2][0]);
    const float3 c1 = float3(world[0][1], world[1][1], world[2][1]);
    const float3 c2 = float3(world[0][2], world[1][2], world[2][2]);
    const float3 offset = position - float3(world[0][3], world[1][3], world[2][3]);
    const float3 r0 = cross(c1, c2);
    return float3(dot(r0, offset), dot(cross(c2, c0), offset), dot(cross(c0, c1), offset)) / dot(c0, r0);
}

bool isOutsideFrustum(GpuCullView view, float3 center, float radius)
{
    [unroll]
    for (uint i = 0; i < 6; ++i)
    {
        if (dot(view.frustumPlanes[i].xyz, center) + view.frustumPlanes[i].w < -radius)
        {
            return true;
        }
    }
    return false;
}

// meshoptimizer sphere form: no apex needed, no singularity at the center
bool isConeBackfacing(GpuMeshletDesc meshlet, float3 objectCamera)
{
    const float3 axis = float3(float(meshlet.coneAxisX), float(meshlet.coneAxisY), float(meshlet.coneAxisZ)) / 127.0;
    const float cutoff = float(meshlet.coneCutoff) / 127.0;
    const float3 offset = meshlet.boundingSphere.xyz - objectCamera;
    return dot(offset, axis) >= cutoff * length(offset) + meshlet.boundingSphere.w;
}

// one atomic per wave instead of per lane
void waveCount(inout uint counter, bool predicate)
{
    const uint count = WaveActiveCountBits(predicate);
    if (WaveIsFirstLane() && count > 0)
    {
        InterlockedAdd(counter, count);
    }
}

uint waveAppend(inout uint counter, bool predicate)
{
    const uint count = WaveActiveCountBits(predicate);
    uint base = 0;
    if (WaveIsFirstLane() && count > 0)
    {
        InterlockedAdd(counter, count, base);
    }
    return WaveReadLaneFirst(base) + WavePrefixCountBits(predicate);
}

groupshared uint instanceRecordBase;

// one workgroup per node instance: a visible instance appends its draw record templates, all lanes copying
[shader("compute")]
[numthreads(kCullGroupSize, 1, 1)]
void cullInstances(uint3 groupId : SV_GroupID, uint lane : SV_GroupIndex)
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    const GpuCullView view = bdaPtr<GpuCullView>(push.cullView)[0];
    GpuCullCounters* counters = bdaPtr<GpuCullCounters>(view.counters);
    const GpuInstanceCullItem item = bdaPtr<GpuInstanceCullItem>(view.instanceItems)[groupId.x];

    // group-uniform: every lane tests the same sphere
    if ((view.flags & kCullInstanceFrustum) != 0 && (item.flags & kDrawRecordSkipCulling) == 0)
    {
        const float4 sphere = bdaPtr<GpuNodeInstance>(root.nodeInstances)[item.nodeInstanceIndex].boundingSphere;
        if (isOutsideFrustum(view, sphere.xyz, sphere.w))
        {
            if (lane == 0 && (view.flags & kCullStats) != 0)
            {
                InterlockedAdd(counters.instancesCulled, 1);
            }
            return;
        }
    }

    // the dispatch x of the meshlet cull doubles as the append counter
    if (lane == 0)
    {
        InterlockedAdd(counters.meshletDispatch[0], item.drawRecordCount, instanceRecordBase);
    }
    GroupMemoryBarrierWithGroupSync();

    const uint base = instanceRecordBase;
    GpuDrawRecord* templates = bdaPtr<GpuDrawRecord>(view.drawRecordTemplates) + item.firstDrawRecord;
    GpuDrawRecord* records = bdaPtr<GpuDrawRecord>(root.drawRecords) + base;
    for (uint r = lane; r < item.drawRecordCount; r += kCullGroupSize)
    {
        records[r] = templates[r];
    }
}

// one workgroup per draw record, one lane per meshlet; dispatched indirectly from GpuCullCounters.meshletDispatch
[shader("compute")]
[numthreads(kCullGroupSize, 1, 1)]
void cullMeshlets(uint3 groupId : SV_GroupID, uint lane : SV_GroupIndex)
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    const GpuCullView view = bdaPtr<GpuCullView>(push.cullView)[0];
    GpuCullCounters* counters = bdaPtr<GpuCullCounters>(view.counters);

    const uint recordIndex = groupId.x;
    const GpuDrawRecord record = bdaPtr<GpuDrawRecord>(root.drawRecords)[recordIndex];
    const GpuPrimitive primitive = bdaPtr<GpuPrimitive>(root.primitives)[record.primitiveIndex];
    const uint group = record.flags & kDrawRecordStateGroupMask;
    const uint meshletIndex = record.meshletOffset + lane;

    const bool isPresent = meshletIndex < primitive.meshletCount;
    const bool isTested = isPresent && (record.flags & kDrawRecordSkipCulling) == 0;
    bool isFrustumCulled = false;
    bool isConeCulled = false;
    if (isTested)
    {
        GpuNodeInstance* instance = bdaPtr<GpuNodeInstance>(root.nodeInstances) + record.nodeInstanceIndex;
        const float4x4 world = instance.worldMatrix;
        const uint64_t meshlets = bdaPtr<GpuModel>(root.models)[instance.modelIndex].meshlets;
        const GpuMeshletDesc meshlet = bdaPtr<GpuMeshletDesc>(meshlets)[primitive.firstMeshlet + meshletIndex];

        if ((view.flags & kCullFrustum) != 0)
        {
            const float3 center = mul(world, float4(meshlet.boundingSphere.xyz, 1.0)).xyz;
            isFrustumCulled = isOutsideFrustum(view, center, meshlet.boundingSphere.w * maxAxisScale(world));
        }
        // double-sided faces are visible from behind
        if (!isFrustumCulled && (view.flags & kCullCone) != 0 && group != kMeshStateGroupDoubleSided)
        {
            isConeCulled = isConeBackfacing(meshlet, worldToObject(world, view.cameraPos));
        }
    }

    const bool isVisible = isPresent && !isFrustumCulled && !isConeCulled;
    // every lane of a group shares the record, so the state group is wave-uniform
    const uint slot = waveAppend(counters.visibleMeshlets[group], isVisible);
    if (isVisible)
    {
        bdaPtr<uint>(view.visibleMeshlets)[view.groupVisibleOffset[group] + slot] =
            (recordIndex << kCullLaneBits) | lane;
    }

    if ((view.flags & kCullStats) != 0)
    {
        waveCount(counters.testedMeshlets, isTested);
        waveCount(counters.frustumCulled, isFrustumCulled);
        waveCount(counters.coneCulled, isConeCulled);
        waveCount(counters.unculledMeshlets, isPresent && !isTested);
    }
}

// after cullMeshlets: one sequence per maxMeshletsPerSequence visible meshlets of each state group
[shader("compute")]
[numthreads(kCullGroupSize, 1, 1)]
void buildMeshSequences(uint lane : SV_GroupIndex)
{
    const GpuCullView view = bdaPtr<GpuCullView>(push.cullView)[0];
    GpuCullCounters* counters = bdaPtr<GpuCullCounters>(view.counters);
    MeshDgcSequence* sequences = bdaPtr<MeshDgcSequence>(view.sequences);
    const uint perSequence = view.maxMeshletsPerSequence;

    [unroll]
    for (uint group = 0; group < kMeshStateGroupCount; ++group)
    {
        const uint visibleCount = counters.visibleMeshlets[group];
        const uint sequenceCount = (visibleCount + perSequence - 1) / perSequence;
        for (uint s = lane; s < sequenceCount; s += kCullGroupSize)
        {
            const uint first = s * perSequence;
            const uint batch = min(visibleCount - first, perSequence);
            MeshDgcSequence sequence;
            sequence.push.frameRoot = push.frameRoot;
            sequence.push.firstVisible = view.groupVisibleOffset[group] + first;
            sequence.push.meshletCount = batch;
            sequence.groupCount = uint3(batch, 1, 1);
            sequence.pad0 = 0;
            sequences[view.groupSequenceOffset[group] + s] = sequence;
        }
        if (lane == 0)
        {
            counters.sequenceCount[group] = sequenceCount;
        }
    }
}
