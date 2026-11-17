// SHELVED with SER (considerations C-G1-3): kept in the tree, not built; the frame traces shaded rays with traceClosest
import types;
import core.binding;
import core.frame;
import geometry.hit;
import raytracing.ser;

// SER self-test, ENGINE_RT_CHECK only (spec/m4/g1_ser_rt_pipeline.md §4): every pixel's primary ray through the SER
// pipeline and through traceClosest. Both run the same traversal, so hit status, ids, barycentrics, hit distance,
// positions and transforms must match bit for bit. One pipeline per reorder mode; writes only GpuSerCheckStats

[[vk::push_constant]] ConstantBuffer<SerCheckPushData> push;

// one atomic per lane, not per wave: subgroup ops after ReorderThread miscount or lose the device on driver 621.01
// (considerations C-G1-2)
void laneCount(inout uint counter, bool predicate)
{
    if (predicate)
    {
        InterlockedAdd(counter, 1u);
    }
}

void laneMax(inout uint counterBits, float value)
{
    if (value > 0.0)
    {
        InterlockedMax(counterBits, asuint(value));
    }
}

bool isSameData(RayHit a, RayHit b)
{
    bool isSame = all(a.barycentrics == b.barycentrics) && a.t == b.t;
    [unroll]
    for (uint i = 0; i < 3; ++i)
    {
        isSame = isSame && all(a.objectPos[i] == b.objectPos[i]);
    }
    [unroll]
    for (uint row = 0; row < 3; ++row)
    {
        isSame = isSame && all(a.objectToWorld[row] == b.objectToWorld[row]) &&
            all(a.worldToObject[row] == b.worldToObject[row]);
    }
    return isSame;
}

[shader("raygeneration")]
void serCheckRaygen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    const GpuTargets targets = loadTargets(root);
    const PrimaryRay primary = primaryRay(loadCamera(root), pixel, float2(targets.renderSize));

    // the SER trace first, so only the ray lives across the reorder
    RayHit shaded;
    const bool isShadedHit = traceShaded(root.tlas, primary.ray, shaded);
    RayHit expected;
    const bool isExpectedHit = traceClosest(root, primary.ray, expected);

    const bool isBothHit = isShadedHit && isExpectedHit;
    const bool isSameIds = isBothHit && shaded.rayInstance == expected.rayInstance &&
        shaded.meshletRow == expected.meshletRow && shaded.triangle == expected.triangle;
    const float2 barycentricError = abs(shaded.barycentrics - expected.barycentrics);
    const float distanceError = abs(shaded.t - expected.t) / max(expected.t, 1e-30);

    GpuSerCheckStats* stats = bdaPtr<GpuSerCheckStats>(push.stats);
    laneCount(stats.rays, true);
    laneCount(stats.hits, isExpectedHit);
    laneCount(stats.hitMismatches, isShadedHit != isExpectedHit);
    laneCount(stats.idMismatches, isBothHit && !isSameIds);
    laneCount(stats.dataMismatches, isSameIds && !isSameData(shaded, expected));
    laneMax(stats.maxBarycentricErrorBits, isSameIds ? max(barycentricError.x, barycentricError.y) : 0.0);
    laneMax(stats.maxDistanceErrorBits, isSameIds ? distanceError : 0.0);
}

[shader("closesthit")]
void serCheckClosestHit(inout ClusterPayload payload, BuiltInTriangleIntersectionAttributes attributes)
{
    writeClusterPayload(payload);
}

// misses are not invoked; the SBT still needs the record
[shader("miss")]
void serCheckMiss(inout ClusterPayload payload)
{
}
