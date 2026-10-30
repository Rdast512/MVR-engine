#include "types.slang"
#include "core/binding.slang"
#include "core/hiz.slang"

// instances -> draw records -> per state group visible lists -> DGC sequences + counts; run once per CullPhase

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

// sphere in mesh space under world, against the view's pyramid and occlusionViewProj
bool isSphereOccluded(GpuCullView view, float4x4 world, float4 sphere)
{
    const float3 center = mul(world, float4(sphere.xyz, 1.0)).xyz;
    const float radius = sphere.w * maxAxisScale(world);
    const HiZFootprint footprint = hizFootprint(view.occlusionViewProj, center, radius, view.hizSize);
    return isHiZOccluded(footprint, view.hizPyramid, view.hizMaxSampler);
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

// early: one workgroup per instance item; occluded ones become late candidates.
// late: one per candidate (dispatched indirectly), re-tested against the fresh pyramid.
// a visible instance appends its draw record templates, all lanes copying
[shader("compute")]
[numthreads(kCullGroupSize, 1, 1)]
void cullInstances(uint3 groupId : SV_GroupID, uint lane : SV_GroupIndex)
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    const GpuCullView view = bdaPtr<GpuCullView>(push.cullView)[0];
    GpuCullCounters* counters = bdaPtr<GpuCullCounters>(view.counters);
    const bool isLate = (view.flags & kCullLatePhase) != 0;
    const bool isStats = (view.flags & kCullStats) != 0;
    const uint itemIndex = isLate ? bdaPtr<uint>(view.instanceCandidates)[groupId.x] : groupId.x;
    const GpuInstanceCullItem item = bdaPtr<GpuInstanceCullItem>(view.instanceItems)[itemIndex];

    // group-uniform: every lane tests the same sphere
    if ((item.flags & kDrawRecordSkipCulling) == 0)
    {
        GpuNodeInstance* instance = bdaPtr<GpuNodeInstance>(root.nodeInstances) + item.nodeInstanceIndex;
        // candidates already passed it
        if (!isLate && (view.flags & kCullInstanceFrustum) != 0)
        {
            const float4 sphere = instance.boundingSphere;
            if (isOutsideFrustum(view, sphere.xyz, sphere.w))
            {
                if (lane == 0 && isStats)
                {
                    InterlockedAdd(counters.instancesCulled, 1);
                }
                return;
            }
        }
        if ((view.flags & kCullOcclusion) != 0)
        {
            // early: where the instance was when the previous pyramid was drawn
            const float4 sphere = isLate ? instance.boundingSphere : instance.prevBoundingSphere;
            const HiZFootprint footprint = hizFootprint(view.occlusionViewProj, sphere.xyz, sphere.w, view.hizSize);
            if (isHiZOccluded(footprint, view.hizPyramid, view.hizMaxSampler))
            {
                if (lane == 0)
                {
                    if (!isLate)
                    {
                        // capacity is the instance count, so the list cannot overflow
                        uint slot;
                        InterlockedAdd(bdaPtr<GpuCullCounters>(view.lateCounters).instanceDispatch[0], 1, slot);
                        bdaPtr<uint>(view.instanceCandidates)[slot] = itemIndex;
                    }
                    if (isStats)
                    {
                        InterlockedAdd(counters.instancesOccluded, 1);
                    }
                }
                return;
            }
        }
    }

    // the dispatch x of the meshlet cull doubles as the append counter
    if (lane == 0)
    {
        InterlockedAdd(counters.meshletDispatch[0], item.drawRecordCount, instanceRecordBase);
        if (isStats)
        {
            InterlockedAdd(counters.instancesDrawn, 1);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    const uint base = view.drawRecordBase + instanceRecordBase;
    GpuDrawRecord* templates = bdaPtr<GpuDrawRecord>(view.drawRecordTemplates) + item.firstDrawRecord;
    GpuDrawRecord* records = bdaPtr<GpuDrawRecord>(root.drawRecords) + base;
    for (uint r = lane; r < item.drawRecordCount; r += kCullGroupSize)
    {
        records[r] = templates[r];
    }
}

// one workgroup per draw record of the phase, one lane per meshlet; dispatched indirectly from
// GpuCullCounters.meshletDispatch
[shader("compute")]
[numthreads(kCullGroupSize, 1, 1)]
void cullMeshlets(uint3 groupId : SV_GroupID, uint lane : SV_GroupIndex)
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    const GpuCullView view = bdaPtr<GpuCullView>(push.cullView)[0];
    GpuCullCounters* counters = bdaPtr<GpuCullCounters>(view.counters);

    const bool isLate = (view.flags & kCullLatePhase) != 0;
    const uint recordIndex = view.drawRecordBase + groupId.x;
    const GpuDrawRecord record = bdaPtr<GpuDrawRecord>(root.drawRecords)[recordIndex];
    const GpuPrimitive primitive = bdaPtr<GpuPrimitive>(root.primitives)[record.primitiveIndex];
    const uint group = record.flags & kDrawRecordStateGroupMask;
    const uint meshletIndex = record.meshletOffset + lane;

    const bool isPresent = meshletIndex < primitive.meshletCount;
    const bool isTested = isPresent && (record.flags & kDrawRecordSkipCulling) == 0;
    bool isFrustumCulled = false;
    bool isConeCulled = false;
    bool isOccluded = false;
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
        // early: where the meshlet was when the previous pyramid was drawn
        if (!isFrustumCulled && !isConeCulled && (view.flags & kCullOcclusion) != 0)
        {
            isOccluded = isSphereOccluded(view, isLate ? world : instance.prevWorldMatrix, meshlet.boundingSphere);
        }
    }

    const uint entry = (recordIndex << kCullLaneBits) | lane;
    const bool isVisible = isPresent && !isFrustumCulled && !isConeCulled && !isOccluded;
    // every lane of a group shares the record, so the state group is wave-uniform
    const uint slot = waveAppend(counters.visibleMeshlets[group], isVisible);
    if (isVisible)
    {
        bdaPtr<uint>(view.visibleMeshlets)[view.groupVisibleOffset[group] + slot] = entry;
    }

    // capacity is the scene's meshlet total and each meshlet is tested once early, so the list cannot overflow
    if (!isLate)
    {
        GpuCullCounters* lateCounters = bdaPtr<GpuCullCounters>(view.lateCounters);
        const uint candidate = waveAppend(lateCounters.meshletCandidateCount, isOccluded);
        if (isOccluded)
        {
            bdaPtr<uint>(view.meshletCandidates)[candidate] = entry;
            // the first candidate of every kCullGroupSize opens a late workgroup
            if (candidate % kCullGroupSize == 0)
            {
                InterlockedAdd(lateCounters.meshletCandidateDispatch[0], 1);
            }
        }
    }

    if ((view.flags & kCullStats) != 0)
    {
        waveCount(counters.testedMeshlets, isTested);
        waveCount(counters.frustumCulled, isFrustumCulled);
        waveCount(counters.coneCulled, isConeCulled);
        waveCount(counters.unculledMeshlets, isPresent && !isTested);
        waveCount(counters.occlusionCulled, isOccluded);
    }
}

// late only: one lane per early-occluded meshlet of an early-drawn instance; frustum and cone already passed,
// so only the occlusion test is redone, with current transforms against the fresh pyramid
[shader("compute")]
[numthreads(kCullGroupSize, 1, 1)]
void cullMeshletCandidates(uint3 id : SV_DispatchThreadID)
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    const GpuCullView view = bdaPtr<GpuCullView>(push.cullView)[0];
    GpuCullCounters* counters = bdaPtr<GpuCullCounters>(view.counters);

    const bool isPresent = id.x < counters.meshletCandidateCount;
    uint entry = 0;
    uint group = 0;
    bool isOccluded = false;
    if (isPresent)
    {
        entry = bdaPtr<uint>(view.meshletCandidates)[id.x];
        const GpuDrawRecord record = bdaPtr<GpuDrawRecord>(root.drawRecords)[entry >> kCullLaneBits];
        const GpuPrimitive primitive = bdaPtr<GpuPrimitive>(root.primitives)[record.primitiveIndex];
        GpuNodeInstance* instance = bdaPtr<GpuNodeInstance>(root.nodeInstances) + record.nodeInstanceIndex;
        const uint64_t meshlets = bdaPtr<GpuModel>(root.models)[instance.modelIndex].meshlets;
        const uint meshletIndex = primitive.firstMeshlet + record.meshletOffset + (entry & kCullLaneMask);
        const GpuMeshletDesc meshlet = bdaPtr<GpuMeshletDesc>(meshlets)[meshletIndex];
        group = record.flags & kDrawRecordStateGroupMask;
        isOccluded = isSphereOccluded(view, instance.worldMatrix, meshlet.boundingSphere);
    }

    const bool isVisible = isPresent && !isOccluded;
    // candidates mix records, so the state group varies per lane: one append per group
    [unroll]
    for (uint g = 0; g < kMeshStateGroupCount; ++g)
    {
        const bool isInGroup = isVisible && group == g;
        const uint slot = waveAppend(counters.visibleMeshlets[g], isInGroup);
        if (isInGroup)
        {
            bdaPtr<uint>(view.visibleMeshlets)[view.groupVisibleOffset[g] + slot] = entry;
        }
    }

    if ((view.flags & kCullStats) != 0)
    {
        waveCount(counters.occlusionCulled, isOccluded);
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
