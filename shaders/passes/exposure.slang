import types;
import core.binding;
import core.frame;

// auto exposure (PBR plan H2, engine spec §12.3): a log2-luminance histogram of the DLSS-RR output, then one group
// that averages the histogram between two percentiles and adapts the exposure image toward key / average. The
// exposure image is read by the next frame's DLSS-RR evaluation and by this frame's post pass

static const uint kHistogramTile = 16; // 256 threads: one per bin
static const float kKeyValue = 0.18; // middle grey after exposure
// share of the non-black pixels ignored at the dark and the bright end
static const float kLowPercentile = 0.40;
static const float kHighPercentile = 0.95;
static const float3 kLuminanceWeights = float3(0.2126, 0.7152, 0.0722); // Rec. 709

[[vk::push_constant]] ConstantBuffer<ExposurePushData> push;

groupshared uint localBins[kExposureHistogramBins];

// bin 0 holds black pixels, which do not count; bins 1.. cover kExposureMinLog2 + [0, kExposureLog2Range)
uint luminanceBin(float luminance)
{
    if (!(luminance >= exp2(kExposureMinLog2)))
    {
        return 0;
    }
    const float t = saturate((log2(luminance) - kExposureMinLog2) / kExposureLog2Range);
    return 1 + min(uint(t * float(kExposureHistogramBins - 1)), kExposureHistogramBins - 2);
}

float binCenterLog2(uint bin)
{
    return kExposureMinLog2 + (float(bin - 1) + 0.5) / float(kExposureHistogramBins - 1) * kExposureLog2Range;
}

[shader("compute")]
[numthreads(kHistogramTile, kHistogramTile, 1)]
void exposureHistogramMain(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    localBins[index] = 0;
    GroupMemoryBarrierWithGroupSync();
    const GpuTargets targets = loadTargets(bdaPtr<GpuFrameRoot>(push.frameRoot)[0]);
    if (all(id.xy < targets.displaySize))
    {
        const float3 color = tex2D(targets.upscaledSampled).Load(int3(id.xy, 0)).rgb;
        InterlockedAdd(localBins[luminanceBin(dot(color, kLuminanceWeights))], 1);
    }
    GroupMemoryBarrierWithGroupSync();
    if (localBins[index] > 0)
    {
        uint* bins = bdaPtr<uint>(push.histogram);
        InterlockedAdd(bins[index], localBins[index]);
    }
}

groupshared uint counts[kExposureHistogramBins];

[shader("compute")]
[numthreads(kExposureHistogramBins, 1, 1)]
void exposureAdaptMain(uint index : SV_GroupIndex)
{
    // read and clear: the next frame's histogram starts from zero
    uint* bins = bdaPtr<uint>(push.histogram);
    counts[index] = bins[index];
    bins[index] = 0;
    GroupMemoryBarrierWithGroupSync();
    if (index != 0)
    {
        return;
    }

    uint total = 0;
    for (uint bin = 1; bin < kExposureHistogramBins; ++bin)
    {
        total += counts[bin];
    }
    // mean log2 luminance of the pixels between the two percentiles
    const float low = float(total) * kLowPercentile;
    const float high = float(total) * kHighPercentile;
    float below = 0.0;
    float weight = 0.0;
    float sum = 0.0;
    for (uint bin = 1; bin < kExposureHistogramBins; ++bin)
    {
        const float count = float(counts[bin]);
        const float inside = max(0.0, min(below + count, high) - max(below, low));
        sum += inside * binCenterLog2(bin);
        weight += inside;
        below += count;
    }

    const GpuTargets targets = loadTargets(bdaPtr<GpuFrameRoot>(push.frameRoot)[0]);
    RWTexture2D<float, 0, kImageFormatR32f> exposureImage = storageTex2D<float, kImageFormatR32f>(targets.exposureStorage);
    const float previous = exposureImage[uint2(0, 0)];
    const bool hasPrevious = previous > 0.0 && !isinf(previous) && !isnan(previous);
    const bool isAuto = (push.flags & kExposureFlagAuto) != 0;
    const float averageLog2 = weight > 0.0 ? sum / weight : log2(kKeyValue);
    float targetLog2 = isAuto ? log2(kKeyValue) - averageLog2 + push.compensationEv : push.manualEv;
    targetLog2 = clamp(targetLog2, push.minEv, push.maxEv);

    float exposureLog2 = targetLog2;
    // no pixels (all black) keep the current exposure; manual and snap jump straight to the target
    if (isAuto && hasPrevious && (push.flags & kExposureFlagSnap) == 0)
    {
        const float previousLog2 = log2(previous);
        exposureLog2 = weight > 0.0
            ? lerp(previousLog2, targetLog2, 1.0 - exp(-push.deltaSeconds * push.adaptationRate))
            : previousLog2;
    }
    const float exposure = exp2(exposureLog2);
    exposureImage[uint2(0, 0)] = exposure;

    GpuExposureReadback* readback = bdaPtr<GpuExposureReadback>(push.readback);
    readback.exposure = exposure;
    readback.targetExposure = exp2(targetLog2);
    readback.averageLuminance = weight > 0.0 ? exp2(averageLog2) : 0.0;
    readback.pixels = total;
}
