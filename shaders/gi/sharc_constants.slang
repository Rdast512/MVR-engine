#ifndef ENGINE_GI_SHARC_CONSTANTS_SLANG
#define ENGINE_GI_SHARC_CONSTANTS_SLANG

// Mirrors GpuSharcConstants, GpuSharcCounters and SharcFlag in src/render/vk_sharc.hpp (scalar layout; layout_test)

static const uint kSharcFlagMaterialDemodulation = 1u << 0;
static const uint kSharcFlagDebugModeShift = 4u;
static const uint kSharcFlagDebugModeMask = 0x7u << kSharcFlagDebugModeShift;

static const uint kSharcDebugCachedRadiance = 0u;
static const uint kSharcDebugColoredHash = 1u;
static const uint kSharcDebugOccupancy = 2u;
static const uint kSharcDebugHashCollisions = 3u;
static const uint kSharcDebugVoxelSize = 4u;
static const uint kSharcDebugGridLevel = 5u;

// Device-local counters copied to host-cached readback after sharc_resolve (mirrors GpuSharcCounters, 8 B)
struct GpuSharcCounters
{
    uint occupied;       // valid non-empty hash-table entries after sharc_resolve
    uint insertFailures; // HashGridInsertEntry bucket overflows during sharc_update
};

struct GpuSharcConstants
{
    uint64_t hashEntries;
    uint64_t accumulation;
    uint64_t resolved;
    float3 cameraPos;
    float sceneScale;
    float3 prevCameraPos;
    float radianceScale;
    uint capacity;
    uint downscaleFactor;
    uint accumulationFrameNum;
    uint staleFrameNumMax;
    uint maxUpdateBounces;
    uint maxQueryBounces;
    float minRoughnessOverride;
    float fireflyClamp;
    uint flags;
    uint updateLaunch;
    uint64_t counters; // GpuSharcCounters* (device-local, CounterReadback)
    uint updateLocalLightSamples; // 0: GpuRtxdiConstants::pathLocalLightSamples
    uint pad0;
};

#endif
