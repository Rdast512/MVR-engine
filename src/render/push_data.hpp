#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <glm/vec3.hpp>
#include <vulkan/vulkan.hpp>

#include "core/types.hpp"

// one DGC sequence: everything else is reached through the frame root
struct MeshPushData {
    vk::DeviceAddress frameRoot;
    // row of GpuFrameRoot::visibleMeshlets drawn by mesh workgroup 0
    uint32_t firstVisible;
    // groups in this batch
    uint32_t meshletCount;
};

// MeshPushData must match shaders/types.slang MeshPushData (16 bytes).
static_assert(std::is_trivially_copyable_v<MeshPushData>);
static_assert(offsetof(MeshPushData, frameRoot) == 0);
static_assert(offsetof(MeshPushData, firstVisible) == 8);
static_assert(offsetof(MeshPushData, meshletCount) == 12);
static_assert(sizeof(MeshPushData) == 16);

// written by buildMeshSequences (shaders/types.slang MeshDgcSequence); packed size is 28, the 32 B stride keeps
// the frame root BDA in pushData 8-byte aligned for every sequence
struct alignas(16) MeshDgcSequence
{
    MeshPushData pushData;
    vk::DrawMeshTasksIndirectCommandEXT draw;
};

static_assert(std::is_trivially_copyable_v<MeshDgcSequence>);
static_assert(offsetof(MeshDgcSequence, pushData) == 0);
static_assert(offsetof(MeshDgcSequence, draw) == sizeof(MeshPushData));
static_assert(sizeof(MeshDgcSequence) == 32);

// one material bin's resolve shader (passes/resolve.slang); must match shaders/types.slang ResolvePushData
struct ResolvePushData {
    vk::DeviceAddress frameRoot;
    // uint[tileCount] of the bin: tile x in the low 16 bits, y in the high
    vk::DeviceAddress tileList;
};

static_assert(std::is_trivially_copyable_v<ResolvePushData>);
static_assert(sizeof(ResolvePushData) == 16);

// written by passes/classify.slang binArgsMain (shaders/types.slang ResolveDgcSequence): EXECUTION_SET + PUSH_DATA +
// DISPATCH; the 40 B stride keeps the push data's addresses 8-byte aligned behind the 16 B GpuMaterialBins counts
struct ResolveDgcSequence
{
    // MaterialBin: index into the resolve execution set
    uint32_t executionSetIndex;
    uint32_t pad0;
    ResolvePushData pushData;
    vk::DispatchIndirectCommand dispatch;
    uint32_t pad1;
};

static_assert(std::is_trivially_copyable_v<ResolveDgcSequence>);
static_assert(offsetof(ResolveDgcSequence, pushData) == 8);
static_assert(offsetof(ResolveDgcSequence, dispatch) == 24);
static_assert(sizeof(ResolveDgcSequence) == 40);

// head of RenderTargets' material-bin buffer, followed by kMaterialBinCount tile lists of one entry per render tile
// each; must match shaders/types.slang GpuMaterialBins. Counts are cleared by a fill before classify
struct GpuMaterialBins
{
    std::array<uint32_t, kMaterialBinCount> tileCount;
    // DGC sequenceCountAddress: one sequence per non-empty bin
    uint32_t sequenceCount;
    std::array<ResolveDgcSequence, kMaterialBinCount> sequences;
};

static_assert(std::is_trivially_copyable_v<GpuMaterialBins>);
static_assert(offsetof(GpuMaterialBins, sequenceCount) == 12);
static_assert(offsetof(GpuMaterialBins, sequences) == 16);
static_assert(sizeof(GpuMaterialBins) == 136);
// the counts the fill clears
inline constexpr vk::DeviceSize kMaterialBinCountBytes = offsetof(GpuMaterialBins, sequences);

// meshlet cull dispatches; must match shaders/types.slang CullPushData
struct CullPushData {
    vk::DeviceAddress frameRoot;
    // GpuCullView of the frame slot
    vk::DeviceAddress cullView;
};

static_assert(std::is_trivially_copyable_v<CullPushData>);
static_assert(sizeof(CullPushData) == 16);

// rt_instances pass; must match shaders/types.slang RtInstancesPushData
struct RtInstancesPushData {
    vk::DeviceAddress frameRoot;
    // GpuRayInstanceBuild[count], parallel to GpuFrameRoot::rayInstances
    vk::DeviceAddress buildRows;
    // VkPartitionedAccelerationStructureWriteInstanceDataNV[count], the PTLAS write-instance arguments
    vk::DeviceAddress writeInfos;
    // the slot's VkBuildPartitionedAccelerationStructureIndirectCommandNV; the pass appends to its argCount
    vk::DeviceAddress operation;
    // one state row per instance (the transform the PTLAS holds), kept with the PTLAS across frames
    vk::DeviceAddress ptlasState;
    uint32_t count;
    uint32_t flags; // RtInstancesFlag
};

static_assert(std::is_trivially_copyable_v<RtInstancesPushData>);
static_assert(offsetof(RtInstancesPushData, count) == 40);
static_assert(sizeof(RtInstancesPushData) == 48);

namespace RtInstancesFlag
{
    // the PTLAS is built from nothing: every row writes its instance and resets its state
    inline constexpr uint32_t FullBuild = 1u << 0;
} // namespace RtInstancesFlag

// matches shaders/passes/hiz_build.slang
inline constexpr uint32_t kHiZLevelsPerDispatch = 3;

// resolve, debug view and present; must match shaders/types.slang FramePushData
struct FramePushData {
    vk::DeviceAddress frameRoot;
};

static_assert(sizeof(FramePushData) == 8);

// light_world pass; must match shaders/types.slang LightWorldPushData
struct LightWorldPushData {
    vk::DeviceAddress frameRoot;
    // world space, the direction the sun's light travels (glTF: L = -direction)
    glm::vec3 sunDirection;
    // lux
    float sunIlluminance;
    glm::vec3 sunColor;
    // rows of the light asset buffer, with the triangle region after their point and spot rows; the sun, when
    // enabled, follows them
    uint32_t assetLightCount;
    // GpuTriangleLightRef[triangleLightRefCount] (RayTracingScene)
    vk::DeviceAddress triangleLightRefs;
    uint32_t triangleLightRefCount;
    // resource heap: storage view of the local light PDF's mip 0 (RtxdiContext)
    uint32_t lightPdf;
    // texels of lightPdf's mip 0, each written every frame: a local light's power at its Z-curve texel, 0 past them
    uint32_t lightPdfTexelCount;
    uint32_t pad0;
};

static_assert(sizeof(LightWorldPushData) == 64);
static_assert(offsetof(LightWorldPushData, sunDirection) == 8);
static_assert(offsetof(LightWorldPushData, assetLightCount) == 36);
static_assert(offsetof(LightWorldPushData, triangleLightRefs) == 40);
static_assert(offsetof(LightWorldPushData, lightPdf) == 52);

// light_pdf_mips pass, one dispatch per level; must match shaders/types.slang LightPdfMipsPushData
struct LightPdfMipsPushData {
    // resource heap: storage views of the read and the written mip
    uint32_t srcImage;
    uint32_t dstImage;
    vk::Extent2D dstSize;
};

static_assert(sizeof(LightPdfMipsPushData) == 16);

// motion_check pass; must match shaders/types.slang MotionCheckPushData
struct MotionCheckPushData {
    vk::DeviceAddress frameRoot;
    // GpuMotionCheckStats
    vk::DeviceAddress stats;
};

static_assert(sizeof(MotionCheckPushData) == 16);

// rt_check pass; must match shaders/types.slang RayCheckPushData
struct RayCheckPushData {
    vk::DeviceAddress frameRoot;
    // GpuRayCheckStats
    vk::DeviceAddress stats;
};

static_assert(sizeof(RayCheckPushData) == 16);

// rt_ser_check pass; must match shaders/types.slang SerCheckPushData
struct SerCheckPushData {
    vk::DeviceAddress frameRoot;
    // GpuSerCheckStats of the pipeline's reorder mode
    vk::DeviceAddress stats;
};

static_assert(sizeof(SerCheckPushData) == 16);

// passes/exposure.slang (both entries); must match shaders/types.slang ExposurePushData
struct ExposurePushData {
    vk::DeviceAddress frameRoot;
    // uint2[kExposureHistogramBins]
    vk::DeviceAddress histogram;
    // GpuExposureReadback of the frame slot
    vk::DeviceAddress readback;
    float deltaSeconds;
    float compensationEv;
    float manualEv;
    float minEv;
    float maxEv;
    float adaptationSpeedUp;
    float adaptationSpeedDown;
    uint32_t flags; // ExposureFlag
};

static_assert(std::is_trivially_copyable_v<ExposurePushData>);
static_assert(offsetof(ExposurePushData, deltaSeconds) == 24);
static_assert(offsetof(ExposurePushData, adaptationSpeedUp) == 44);
static_assert(offsetof(ExposurePushData, flags) == 52);
static_assert(sizeof(ExposurePushData) == 56);

// ExposurePushData::flags; must match shaders/types.slang kExposureFlag*
namespace ExposureFlag
{
    inline constexpr uint32_t Auto = 1u << 0;
    // jump to the target instead of adapting
    inline constexpr uint32_t Snap = 1u << 1;
} // namespace ExposureFlag

// present pass; must match shaders/types.slang PresentPushData
struct PresentPushData {
    vk::DeviceAddress frameRoot;
    // swapchain extent
    vk::Extent2D displaySize;
};

static_assert(sizeof(PresentPushData) == 16);

// hi-z build dispatches; must match shaders/types.slang HiZPushData
struct HiZPushData {
    // resource heap: depth for mip 0, the pyramid's full-chain view otherwise
    uint32_t srcImage;
    // resource heap: storage views of the written mips, first levelCount valid
    std::array<uint32_t, kHiZLevelsPerDispatch> dstImages;
    // pyramid mip the first written level halves; unused from depth
    uint32_t srcMip;
    uint32_t isFromDepth;
    uint32_t levelCount;
    uint32_t pad0;
    vk::Extent2D srcSize;
    // first written level
    vk::Extent2D dstSize;
};

static_assert(std::is_trivially_copyable_v<HiZPushData>);
static_assert(offsetof(HiZPushData, srcSize) == 32);
static_assert(sizeof(HiZPushData) == 48);

// Hi-Z pyramid debug view; must match shaders/types.slang HiZViewPushData
struct HiZViewPushData {
    // resource heap, full mip chain
    uint32_t pyramid;
    uint32_t mip;
    float nearZ;
    float farZ;
    vk::Extent2D screenSize;
};

static_assert(std::is_trivially_copyable_v<HiZViewPushData>);
static_assert(sizeof(HiZViewPushData) == 24);

// largest push block of any pass; checked against maxPushDataSize at startup
inline constexpr size_t kMaxPushDataBytes =
    std::max({sizeof(MeshPushData), sizeof(CullPushData), sizeof(HiZPushData), sizeof(HiZViewPushData),
              sizeof(FramePushData), sizeof(ResolvePushData), sizeof(PresentPushData), sizeof(RtInstancesPushData),
              sizeof(ExposurePushData), sizeof(LightWorldPushData), sizeof(LightPdfMipsPushData),
              sizeof(SerCheckPushData)});
