import types;
import core.binding;
import core.frame;

// auto exposure (PBR plan H2, engine spec §12.3): a log2-luminance histogram of the post input (DLSS-RR's output,
// noisyColor or the accumulated mean), then one group that averages the histogram between two percentiles and adapts
// the exposure image toward key / average. The exposure image is read by this frame's post pass and reused as
// adaptation history

static const uint kHistogramTile = 16; // 256 threads: one per bin
static const float kKeyValue = 0.18; // middle grey after exposure
static const uint kFixedPointScale = 64;
static const float kBinLog2Step = kExposureLog2Range / float(kExposureHistogramBins - 1);
static const float kLowPercentile = 0.40;
static const float kHighPercentile = 0.95;
static const float3 kLuminanceWeights = float3(0.2126, 0.7152, 0.0722); // Rec. 709

[[vk::push_constant]] ConstantBuffer<ExposurePushData> push;

groupshared uint localBins[kExposureHistogramBins];

[shader("compute")]
[numthreads(kHistogramTile, kHistogramTile, 1)]
void exposureHistogramMain(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    localBins[index] = 0;
    GroupMemoryBarrierWithGroupSync();
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    const GpuTargets targets = loadTargets(root);
    if (all(id.xy < targets.displaySize))
    {
        float3 color = tex2D(push.input).Load(int3(id.xy, 0)).rgb;
        if (root.exposure != 1.0)
        {
            color /= max(root.exposure, 1e-6);
        }
        const float luminance = dot(color, kLuminanceWeights);
        if (isfinite(luminance))
        {
            const float logLuminance = log2(max(luminance, exp2(kExposureMinLog2)));
            const float bin = saturate((logLuminance - kExposureMinLog2) / kExposureLog2Range) *
                float(kExposureHistogramBins - 1);
            const uint lowerBin = uint(bin);
            const uint upperWeight = uint(frac(bin) * float(kFixedPointScale));
            InterlockedAdd(localBins[lowerBin], kFixedPointScale - upperWeight);
            if (upperWeight != 0)
            {
                InterlockedAdd(localBins[lowerBin + 1], upperWeight);
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (localBins[index] > 0)
    {
        uint2* bins = bdaPtr<uint2>(push.histogram);
        uint previousWeight;
        InterlockedAdd(bins[index].x, localBins[index], previousWeight);
        // keep large extents safe without 64-bit atomics
        if (previousWeight + localBins[index] < previousWeight)
        {
            InterlockedAdd(bins[index].y, 1u);
        }
    }
}

groupshared uint64_t counts[kExposureHistogramBins];

[shader("compute")]
[numthreads(kExposureHistogramBins, 1, 1)]
void exposureAdaptMain(uint index : SV_GroupIndex)
{
    // read and clear: the next frame's histogram starts from zero
    uint2* bins = bdaPtr<uint2>(push.histogram);
    const uint2 binWeight = bins[index];
    counts[index] = (uint64_t(binWeight.y) << 32) | uint64_t(binWeight.x);
    bins[index] = uint2(0);
    GroupMemoryBarrierWithGroupSync();
    if (index != 0)
    {
        return;
    }

    uint64_t total = 0;
    for (uint bin = 0; bin < kExposureHistogramBins; ++bin)
    {
        total += counts[bin];
    }
    // mean log2 luminance of the pixels between the two percentiles
    const float low = float(total) * kLowPercentile;
    const float high = float(total) * kHighPercentile;
    float below = 0.0;
    float weight = 0.0;
    float sum = 0.0;
    for (uint bin = 0; bin < kExposureHistogramBins; ++bin)
    {
        const float count = float(counts[bin]);
        const float inside = max(0.0, min(below + count, high) - max(below, low));
        sum += inside * (kExposureMinLog2 + float(bin) * kBinLog2Step);
        weight += inside;
        below += count;
    }

    const GpuTargets targets = loadTargets(bdaPtr<GpuFrameRoot>(push.frameRoot)[0]);
    RWTexture2D<float, 0, kImageFormatR32f> exposureImage = storageTex2D<float, kImageFormatR32f>(targets.exposureStorage);
    const float previous = exposureImage[uint2(0, 0)];
    const bool hasPrevious = previous > 0.0 && isfinite(previous);
    const bool isAuto = (push.flags & kExposureFlagAuto) != 0;
    const float previousLog2 = clamp(hasPrevious ? log2(previous) : 0.0, push.minEv, push.maxEv);
    const float averageLog2 = weight > 0.0 ? sum / weight : log2(kKeyValue);
    const float autoLog2 = weight > 0.0 ? log2(kKeyValue) - averageLog2 + push.compensationEv : previousLog2;
    const float targetLog2 = clamp(isAuto ? autoLog2 : push.manualEv, push.minEv, push.maxEv);

    float exposureLog2 = targetLog2;
    if (isAuto && hasPrevious && (push.flags & kExposureFlagSnap) == 0)
    {
        // exposure rises as metered luminance falls
        const float speed = targetLog2 < previousLog2 ? push.adaptationSpeedUp : push.adaptationSpeedDown;
        exposureLog2 = lerp(targetLog2, previousLog2, exp2(-push.deltaSeconds * speed));
    }
    const float exposure = exp2(exposureLog2);
    exposureImage[uint2(0, 0)] = exposure;

    GpuExposureReadback* readback = bdaPtr<GpuExposureReadback>(push.readback);
    readback.exposure = exposure;
    readback.targetExposure = exp2(targetLog2);
    readback.averageLuminance = weight > 0.0 ? exp2(averageLog2) : 0.0;
    readback.pixels = uint(total / kFixedPointScale);
}
