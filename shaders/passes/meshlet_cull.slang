import types;
import core.binding;
import core.culling;

// instances -> draw records -> per raster bin visible lists -> DGC sequences + counts; run once per CullPhase

[[vk::push_constant]] ConstantBuffer<CullPushData> push;

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

// object-space box under world, against the view's pyramid and occlusionViewProj
bool isBoxOccluded(GpuCullView view, float4x4 world, float3 bboxMin, float3 bboxMax)
{
    ClipBounds bounds;
    intersectFrustum(view.occlusionViewProj, bboxMin, bboxMax, world, bounds);
    return isHiZOccluded(bounds, view.hizSize, view.hizPyramid, view.hizFarSampler);
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
groupshared uint groupRejectMask;

// one lane's work: the instance tests, the candidate append and the record-base append. Returns the base of the
// instance's draw records in the phase region, or kNoneIndex when the instance is not drawn this phase
uint cullInstance(GpuFrameRoot root, GpuCullView view, uint itemIndex, GpuInstanceCullItem item)
{
    GpuCullCounters* counters = bdaPtr<GpuCullCounters>(view.counters);
    const bool isLate = (view.flags & kCullLatePhase) != 0;
    const bool isStats = (view.flags & kCullStats) != 0;

    // the node's mesh box under the instance matrix
    GpuNodeInstance* instance = bdaPtr<GpuNodeInstance>(root.nodeInstances) + item.nodeInstanceIndex;
    GpuModel* model = bdaPtr<GpuModel>(root.models) + instance.modelIndex;
    GpuNode* node = bdaPtr<GpuNode>(model.nodes) + instance.nodeIndex;
    const float3 bboxMin = node.bboxMin;
    const float3 bboxMax = node.bboxMax;
    // candidates already passed both
    if (!isLate && (view.flags & (kCullInstanceFrustum | kCullSubPixel)) != 0)
    {
        ClipBounds bounds;
        const bool isInFrustum =
            intersectFrustum(view.cullViewProj, bboxMin, bboxMax, instance.worldMatrix, bounds);
        if ((view.flags & kCullInstanceFrustum) != 0 && !isInFrustum)
        {
            if (isStats)
            {
                InterlockedAdd(counters.instancesCulled, 1);
            }
            return kNoneIndex;
        }
        // final, so a sub-pixel instance never becomes a candidate
        if ((view.flags & kCullSubPixel) != 0 && isSubPixel(bounds, view.targetSize, view.subpixelSnap))
        {
            if (isStats)
            {
                InterlockedAdd(counters.instancesSubPixel, 1);
            }
            return kNoneIndex;
        }
    }
    if ((view.flags & kCullOcclusion) != 0)
    {
        // early: where the instance was when the previous pyramid was drawn
        if (isBoxOccluded(view, isLate ? instance.worldMatrix : instance.prevWorldMatrix, bboxMin, bboxMax))
        {
            if (!isLate)
            {
                uint slot;
                InterlockedAdd(bdaPtr<GpuCullCounters>(view.lateCounters).instanceDispatch[0], 1, slot);
                if (slot < view.instanceCandidateCapacity)
                {
                    bdaPtr<uint>(view.instanceCandidates)[slot] = itemIndex;
                }
                else
                {
                    InterlockedAdd(counters.candidatesOverflowed, 1);
                }
            }
            if (isStats)
            {
                InterlockedAdd(counters.instancesOccluded, 1);
            }
            return kNoneIndex;
        }
    }

    // the dispatch x of the meshlet cull doubles as the append counter
    uint recordBase;
    InterlockedAdd(counters.meshletDispatch[0], item.drawRecordCount, recordBase);
    const uint end = recordBase + item.drawRecordCount;
    if (end > view.drawRecordCapacity)
    {
        InterlockedAdd(counters.drawRecordsOverflowed, end - max(recordBase, view.drawRecordCapacity));
    }
    if (isStats)
    {
        InterlockedAdd(counters.instancesDrawn, 1);
    }
    return recordBase;
}

// early: one workgroup per instance item; occluded ones become late candidates.
// late: one per candidate (dispatched indirectly), re-tested against the fresh pyramid.
// lane 0 tests (the verdict is group-uniform); a visible instance's draw record templates are copied by all lanes
[shader("compute")]
[numthreads(kCullGroupSize, 1, 1)]
void cullInstances(uint3 groupId : SV_GroupID, uint lane : SV_GroupIndex)
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    const GpuCullView view = bdaPtr<GpuCullView>(push.cullView)[0];
    const bool isLate = (view.flags & kCullLatePhase) != 0;
    // the late dispatch counts dropped candidates too
    if (isLate && groupId.x >= view.instanceCandidateCapacity)
    {
        return;
    }
    const uint itemIndex = isLate ? bdaPtr<uint>(view.instanceCandidates)[groupId.x] : groupId.x;
    const GpuInstanceCullItem item = bdaPtr<GpuInstanceCullItem>(view.instanceItems)[itemIndex];

    if (lane == 0)
    {
        instanceRecordBase = cullInstance(root, view, itemIndex, item);
    }
    GroupMemoryBarrierWithGroupSync();
    if (instanceRecordBase == kNoneIndex)
    {
        return;
    }

    // records past the region's capacity are dropped (counted in cullInstance)
    const uint fitting = instanceRecordBase >= view.drawRecordCapacity
        ? 0
        : min(item.drawRecordCount, view.drawRecordCapacity - instanceRecordBase);
    const uint base = view.drawRecordBase + instanceRecordBase;
    GpuDrawRecord* templates = bdaPtr<GpuDrawRecord>(view.drawRecordTemplates) + item.firstDrawRecord;
    GpuDrawRecord* records = bdaPtr<GpuDrawRecord>(root.drawRecords) + base;
    for (uint r = lane; r < fitting; r += kCullGroupSize)
    {
        records[r] = templates[r];
    }
}

// one workgroup per draw record of the phase, i.e. per meshlet group, one lane per meshlet; dispatched indirectly from
// GpuCullCounters.meshletDispatch. The group box is tested first: a group verdict holds for all its meshlets, since
// their boxes lie inside it
[shader("compute")]
[numthreads(kMeshletGroupMaxSize, 1, 1)]
void cullMeshlets(uint3 groupId : SV_GroupID, uint lane : SV_GroupIndex)
{
    // one reject-mask bit per lane; cullRejectedMeshlets reads the same width
    static_assert(kMeshletGroupMaxSize <= 32, "reject mask is one uint per group");
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    const GpuCullView view = bdaPtr<GpuCullView>(push.cullView)[0];
    // the dispatch counts dropped records too; group-uniform and before any barrier
    if (groupId.x >= view.drawRecordCapacity)
    {
        return;
    }
    GpuCullCounters* counters = bdaPtr<GpuCullCounters>(view.counters);

    const bool isLate = (view.flags & kCullLatePhase) != 0;
    const uint recordIndex = view.drawRecordBase + groupId.x;
    const GpuDrawRecord record = bdaPtr<GpuDrawRecord>(root.drawRecords)[recordIndex];
    const GpuPrimitive primitive = bdaPtr<GpuPrimitive>(root.primitives)[record.primitiveIndex];
    const uint bin = record.flags & kDrawRecordBinMask;
    const uint meshletIndex = record.meshletOffset + lane;
    GpuNodeInstance* instance = bdaPtr<GpuNodeInstance>(root.nodeInstances) + record.nodeInstanceIndex;
    GpuModel* model = bdaPtr<GpuModel>(root.models) + instance.modelIndex;
    const GpuMeshletGroup meshletGroup =
        bdaPtr<GpuMeshletGroup>(model.meshletGroups)[primitive.firstGroup + record.groupOffset];

    const bool isPresent = lane < meshletGroup.meshletCount;
    const float4x4 world = instance.worldMatrix;
    // early: where the bounds were when the previous pyramid was drawn
    const float4x4 occlusionWorld = isLate ? world : instance.prevWorldMatrix;

    // group-uniform; same order as the meshlet tests below, without the cone
    bool isGroupFrustumCulled = false;
    bool isGroupSubPixel = false;
    bool isGroupOccluded = false;
    ClipBounds groupBounds = clipBoundsBegin();
    if ((view.flags & (kCullFrustum | kCullSubPixel)) != 0)
    {
        const bool isInFrustum =
            intersectFrustum(view.cullViewProj, meshletGroup.bboxMin, meshletGroup.bboxMax, world, groupBounds);
        isGroupFrustumCulled = (view.flags & kCullFrustum) != 0 && !isInFrustum;
    }
    if (!isGroupFrustumCulled && (view.flags & kCullSubPixel) != 0)
    {
        isGroupSubPixel = isSubPixel(groupBounds, view.targetSize, view.subpixelSnap);
    }
    if (!isGroupFrustumCulled && !isGroupSubPixel && (view.flags & kCullOcclusion) != 0)
    {
        isGroupOccluded = isBoxOccluded(view, occlusionWorld, meshletGroup.bboxMin, meshletGroup.bboxMax);
    }

    bool isFrustumCulled = false;
    bool isConeCulled = false;
    bool isSubPixelCulled = false;
    bool isOccluded = false;
    if (isPresent)
    {
        const uint row = primitive.firstMeshlet + meshletIndex;
        const GpuMeshletDesc meshlet = bdaPtr<GpuMeshletDesc>(model.meshlets)[row];
        const GpuMeshletBounds box = bdaPtr<GpuMeshletBounds>(model.meshletBounds)[row];

        // one projection for the frustum and sub-pixel tests, skipped when the group decided both
        ClipBounds bounds = clipBoundsBegin();
        isFrustumCulled = isGroupFrustumCulled;
        if (!isFrustumCulled && !isGroupSubPixel && (view.flags & (kCullFrustum | kCullSubPixel)) != 0)
        {
            const bool isInFrustum = intersectFrustum(view.cullViewProj, box.bboxMin, box.bboxMax, world, bounds);
            isFrustumCulled = (view.flags & kCullFrustum) != 0 && !isInFrustum;
        }
        // double-sided faces are visible from behind
        if (!isFrustumCulled && (view.flags & kCullCone) != 0 && (record.flags & kDrawRecordTwoSided) == 0)
        {
            isConeCulled = isConeBackfacing(meshlet, worldToObject(world, view.cameraPos));
        }
        // final in either phase, so a sub-pixel meshlet never becomes a candidate
        if (!isFrustumCulled && !isConeCulled && (view.flags & kCullSubPixel) != 0)
        {
            isSubPixelCulled = isGroupSubPixel || isSubPixel(bounds, view.targetSize, view.subpixelSnap);
        }
        if (!isFrustumCulled && !isConeCulled && !isSubPixelCulled && (view.flags & kCullOcclusion) != 0)
        {
            isOccluded =
                isGroupOccluded || isBoxOccluded(view, occlusionWorld, box.bboxMin, box.bboxMax);
        }
    }

    const bool isVisible = isPresent && !isFrustumCulled && !isConeCulled && !isSubPixelCulled && !isOccluded;
    // every lane of a group shares the record, so the bin is wave-uniform
    const uint slot = waveAppend(counters.visibleMeshlets[bin], isVisible);
    const bool isStored = isVisible && slot < view.binVisibleCapacity[bin];
    waveCount(counters.visibleOverflowed, isVisible && !isStored);
    if (isStored)
    {
        GpuVisibleMeshlet entry;
        entry.nodeInstanceIndex = record.nodeInstanceIndex;
        entry.meshletRow = primitive.firstMeshlet + meshletIndex;
        bdaPtr<GpuVisibleMeshlet>(view.visibleMeshlets)[view.binVisibleOffset[bin] + slot] = entry;
    }

    // reject list (vk_lod_clusters rejectClusterMasks): one bit per meshlet that passed frustum, cone and sub-pixel
    // but failed occlusion. Written for every early record, zero included, so the mask needs no clear. The
    // reference also drops a mask whose union box left the current frustum; here the early frustum test already
    // used the current (or frozen) matrix the late phase uses, so every set bit is inside it
    if (!isLate)
    {
        if (lane == 0)
        {
            groupRejectMask = 0;
        }
        GroupMemoryBarrierWithGroupSync();
        if (isOccluded)
        {
            InterlockedOr(groupRejectMask, 1u << lane);
        }
        GroupMemoryBarrierWithGroupSync();
        if (lane == 0)
        {
            bdaPtr<uint>(view.rejectMasks)[groupId.x] = groupRejectMask;
        }
    }

    if ((view.flags & kCullStats) != 0)
    {
        if (lane == 0)
        {
            InterlockedAdd(counters.groupsTested, 1);
            if (isGroupFrustumCulled || isGroupSubPixel || isGroupOccluded)
            {
                InterlockedAdd(counters.groupsCulled, 1);
            }
        }
        waveCount(counters.testedMeshlets, isPresent);
        waveCount(counters.frustumCulled, isFrustumCulled);
        waveCount(counters.coneCulled, isConeCulled);
        waveCount(counters.meshletsSubPixel, isSubPixelCulled);
        waveCount(counters.occlusionCulled, isOccluded);
    }
}

// late only (traversal_reject_clusters): one workgroup per early record, dispatched with the early phase's
// meshletDispatch, one lane per meshlet. Only the meshlets set in the record's reject mask are re-tested, and only
// for occlusion: frustum, cone and sub-pixel passed early with the same matrices. Current transforms, fresh pyramid
[shader("compute")]
[numthreads(kMeshletGroupMaxSize, 1, 1)]
void cullRejectedMeshlets(uint3 groupId : SV_GroupID, uint lane : SV_GroupIndex)
{
    const GpuCullView view = bdaPtr<GpuCullView>(push.cullView)[0];
    // the early dispatch counts dropped records too; the reject masks have the same capacity
    if (groupId.x >= view.drawRecordCapacity)
    {
        return;
    }
    const uint rejectMask = bdaPtr<uint>(view.rejectMasks)[groupId.x];
    // most groups are either drawn or culled whole early
    if (rejectMask == 0)
    {
        return;
    }
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    GpuCullCounters* counters = bdaPtr<GpuCullCounters>(view.counters);

    const uint recordIndex = view.rejectRecordBase + groupId.x;
    const GpuDrawRecord record = bdaPtr<GpuDrawRecord>(root.drawRecords)[recordIndex];
    const GpuPrimitive primitive = bdaPtr<GpuPrimitive>(root.primitives)[record.primitiveIndex];
    GpuNodeInstance* instance = bdaPtr<GpuNodeInstance>(root.nodeInstances) + record.nodeInstanceIndex;
    GpuModel* model = bdaPtr<GpuModel>(root.models) + instance.modelIndex;
    const uint bin = record.flags & kDrawRecordBinMask;
    const float4x4 world = instance.worldMatrix;

    // group-uniform: a group box still occluded keeps all its bits occluded
    const GpuMeshletGroup meshletGroup =
        bdaPtr<GpuMeshletGroup>(model.meshletGroups)[primitive.firstGroup + record.groupOffset];
    const bool isGroupOccluded = isBoxOccluded(view, world, meshletGroup.bboxMin, meshletGroup.bboxMax);

    const bool isRejected = (rejectMask & (1u << lane)) != 0;
    bool isOccluded = false;
    if (isRejected)
    {
        const uint row = primitive.firstMeshlet + record.meshletOffset + lane;
        const GpuMeshletBounds box = bdaPtr<GpuMeshletBounds>(model.meshletBounds)[row];
        isOccluded = isGroupOccluded || isBoxOccluded(view, world, box.bboxMin, box.bboxMax);
    }

    const bool isVisible = isRejected && !isOccluded;
    const uint slot = waveAppend(counters.visibleMeshlets[bin], isVisible);
    const bool isStored = isVisible && slot < view.binVisibleCapacity[bin];
    waveCount(counters.visibleOverflowed, isVisible && !isStored);
    if (isStored)
    {
        GpuVisibleMeshlet entry;
        entry.nodeInstanceIndex = record.nodeInstanceIndex;
        entry.meshletRow = primitive.firstMeshlet + record.meshletOffset + lane;
        bdaPtr<GpuVisibleMeshlet>(view.visibleMeshlets)[view.binVisibleOffset[bin] + slot] = entry;
    }

    if ((view.flags & kCullStats) != 0)
    {
        waveCount(counters.meshletsRetested, isRejected);
        waveCount(counters.occlusionCulled, isOccluded);
    }
}

// after cullMeshlets: one sequence per maxMeshletsPerSequence visible meshlets of each raster bin
[shader("compute")]
[numthreads(kCullGroupSize, 1, 1)]
void buildMeshSequences(uint lane : SV_GroupIndex)
{
    const GpuCullView view = bdaPtr<GpuCullView>(push.cullView)[0];
    GpuCullCounters* counters = bdaPtr<GpuCullCounters>(view.counters);
    MeshDgcSequence* sequences = bdaPtr<MeshDgcSequence>(view.sequences);
    const uint perSequence = view.maxMeshletsPerSequence;

    [unroll]
    for (uint bin = 0; bin < kRasterBinCount; ++bin)
    {
        // the counter includes dropped appends
        const uint visibleCount = min(counters.visibleMeshlets[bin], view.binVisibleCapacity[bin]);
        const uint sequenceCount = (visibleCount + perSequence - 1) / perSequence;
        for (uint s = lane; s < sequenceCount; s += kCullGroupSize)
        {
            const uint first = s * perSequence;
            const uint batch = min(visibleCount - first, perSequence);
            MeshDgcSequence sequence;
            sequence.push.frameRoot = push.frameRoot;
            sequence.push.firstVisible = view.binVisibleOffset[bin] + first;
            sequence.push.meshletCount = batch;
            sequence.groupCount = uint3(batch, 1, 1);
            sequence.pad0 = 0;
            sequences[view.binSequenceOffset[bin] + s] = sequence;
        }
        if (lane == 0)
        {
            counters.sequenceCount[bin] = sequenceCount;
        }
    }
}
