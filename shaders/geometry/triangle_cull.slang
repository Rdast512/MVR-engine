#ifndef ENGINE_GEOMETRY_TRIANGLE_CULL_SLANG
#define ENGINE_GEOMETRY_TRIANGLE_CULL_SLANG

#include "types.slang"
#include "core/binding.slang"
#include "core/frame.slang"

// per-triangle culling in the mesh shaders, flagged through SV_CullPrimitive. Corners are the clip positions the
// rasterizer receives, so a projection jitter is accounted for once it lives in proj. Single sample: coverage is
// decided at pixel centers.

static const uint kTriangleKept = 0;
static const uint kTriangleBackface = 1;
static const uint kTriangleSmall = 2;

// tri must already carry the winding the rasterizer sees (loadFrontFacingTriangle)
uint triangleCullReason(float4 a, float4 b, float4 c, uint flags, bool isDoubleSided, float2 targetSize,
                        float subpixelSnap)
{
    // a corner on or behind the eye plane has no valid projection; the clipper handles it
    if (a.w <= 0.0 || b.w <= 0.0 || c.w <= 0.0)
    {
        return kTriangleKept;
    }
    const float2 pa = a.xy / a.w;
    const float2 pb = b.xy / b.w;
    const float2 pc = c.xy / c.w;

    if ((flags & kTriangleCullBackface) != 0 && !isDoubleSided)
    {
        // viewport height is positive, so NDC keeps the framebuffer's orientation. With frontFace = CCW the
        // rasterizer treats a negative value as front-facing; zero area covers nothing
        const float area = (pb.x - pa.x) * (pc.y - pa.y) - (pb.y - pa.y) * (pc.x - pa.x);
        if (area >= 0.0)
        {
            return kTriangleBackface;
        }
    }

    if ((flags & kTriangleCullSmall) != 0)
    {
        const float2 sa = (pa * 0.5 + 0.5) * targetSize;
        const float2 sb = (pb * 0.5 + 0.5) * targetSize;
        const float2 sc = (pc * 0.5 + 0.5) * targetSize;
        // widened by the snap distance: the rasterizer tests the snapped corners, not these
        const float2 lo = min(sa, min(sb, sc)) - subpixelSnap;
        const float2 hi = max(sa, max(sb, sc)) + subpixelSnap;
        // pixel centers sit at k + 0.5; none inside the box on one axis means no coverage
        if (any(floor(hi - 0.5) < ceil(lo - 0.5)))
        {
            return kTriangleSmall;
        }
    }
    return kTriangleKept;
}

// per-lane tallies of one mesh workgroup; one atomic per counter per wave
void countTriangles(DrawContext draw, uint tested, uint backface, uint small)
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

#endif
