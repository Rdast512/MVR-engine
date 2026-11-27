import types;
import core.binding;
import core.counters;
import core.frame;
import core.math;

#include "gi/sharc_bridge.slang"

// SHaRC temporal resolve & eviction pass (M8.4, spec/m8/m8_4_sharc_cache.md §4.2): merges per-frame atomic
// accumulation into resolved FP16 history, runs fade acceleration + adjacent-level blend, evicts stale entries, clears
// accumulationBuffer in place for the next frame, and counts occupied entries (1 atomic/wave) into GpuSharcCounters.

static const uint kSharcResolveGroupSize = 256;

[[vk::push_constant]] ConstantBuffer<FramePushData> push;

[shader("compute")]
[numthreads(kSharcResolveGroupSize, 1, 1)]
void sharcResolveMain(uint3 id : SV_DispatchThreadID)
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    if (root.sharcConstants == 0)
    {
        return;
    }
    const GpuSharcConstants sharc = loadSharcConstants(root);
    const SharcSdkParams sharcParams = sharcSdkParamsFrom(sharc);
    const bool isOccupied = sharcResolveVoxel(id.x, sharcParams, sharc.prevCameraPos, sharc.accumulationFrameNum,
                                              sharc.staleFrameNumMax, root.frameIndex);
    if (sharc.counters != 0 && WaveActiveAnyTrue(isOccupied))
    {
        GpuSharcCounters* counters = bdaPtr<GpuSharcCounters>(sharc.counters);
        waveCount(counters.occupied, isOccupied);
    }
}
