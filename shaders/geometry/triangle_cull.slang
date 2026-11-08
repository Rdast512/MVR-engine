module triangle_cull;

import types;
import core.binding;
import core.frame;
import core.culling;

// per-triangle culling in the mesh shaders, flagged through SV_CullPrimitive. Corners are the clip positions the
// rasterizer receives, so a projection jitter is accounted for once it lives in proj. Single sample: coverage is
// decided at pixel centers. The tests are isFrontFacingHW and testTrianglePixel from core/culling.slang

public static const uint kTriangleKept = 0;
public static const uint kTriangleBackface = 1;
public static const uint kTriangleSmall = 2;

// tri must already carry the winding the rasterizer sees (loadFrontFacingTriangle)
public uint triangleCullReason(float4 a, float4 b, float4 c, uint flags, bool isDoubleSided, float2 targetSize,
                        float subpixelSnap)
{
    // no divide, so valid before the eye-plane guard; zero area counts as front facing, as in vk_lod_clusters
    if ((flags & kTriangleCullBackface) != 0 && !isDoubleSided && !isFrontFacingHW(a, b, c))
    {
        return kTriangleBackface;
    }

    // a corner on or behind the eye plane has no valid projection; the clipper handles it
    if (a.w <= 0.0 || b.w <= 0.0 || c.w <= 0.0)
    {
        return kTriangleKept;
    }

    // the cull passes did the frustum test per meshlet, so no viewport test here
    if ((flags & kTriangleCullSmall) != 0 &&
        !testTrianglePixel(getScreenPos(a, targetSize), getScreenPos(b, targetSize), getScreenPos(c, targetSize),
                           false, targetSize, subpixelSnap))
    {
        return kTriangleSmall;
    }
    return kTriangleKept;
}

// per-lane tallies of one mesh workgroup; one atomic per counter per wave
public void countTriangles(DrawContext draw, uint tested, uint backface, uint small)
{
    const uint waveTested = WaveActiveSum(tested);
    const uint waveBackface = WaveActiveSum(backface);
    const uint waveSmall = WaveActiveSum(small);
    if (WaveIsFirstLane() && waveTested > 0)
    {
        const uint phase = draw.visibleRow >= draw.root.lateVisibleBase ? 1 : 0;
        GpuCullCounters* counters = bdaPtr<GpuCullCounters>(draw.root.cullCounters) + phase;
        InterlockedAdd(counters.trianglesTested, waveTested);
        InterlockedAdd(counters.trianglesBackface, waveBackface);
        InterlockedAdd(counters.trianglesSmall, waveSmall);
    }
}
