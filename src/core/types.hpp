#pragma once

#include <array>
#include <string>
#include <utility>
#include <vector>

struct TextureAsset
{
    vk::raii::Image textureImage = nullptr;
    vk::raii::ImageView textureImageView = nullptr;
    VmaAllocation textureImageMemory = nullptr;
    uint32_t descriptorHeapIndex = 0;
};


// Tight packing: pos@0, color@12, texCoord@24, sizeof==32 (glm). Mesh BDA path requires
// device scalarBlockLayout + slangc -fvk-use-scalar-layout so SPIR-V matches this layout.
struct GpuVertex
{
    glm::vec3 pos;
    glm::vec3 color;
    glm::vec2 texCoord;

    bool operator==(const GpuVertex& other) const
    {
        return pos == other.pos && color == other.color && texCoord == other.texCoord;
    }
};
static_assert(sizeof(GpuVertex) == 32, "GpuVertex must stay 32 B for mesh BDA / scalar layout");
static_assert(offsetof(GpuVertex, color) == 12);
static_assert(offsetof(GpuVertex, texCoord) == 24);


// GPU-friendly meshlet header (CPU layout matches mesh.slang GpuMeshletDesc / SSBO).
struct alignas(16) GpuMeshletDesc
{
    uint32_t vertexOffset = 0; // into meshletVertices
    uint32_t triangleOffset = 0; // into meshletTriangles (first corner index)
    uint8_t vertexCount = 0; // <= 64
    uint8_t triangleCount = 0; // <= 126
    uint8_t pad0 = 0;
    uint8_t pad1 = 0;
    // normal cone in object space, snorm8 (x / 127); cutoff 127 never culls
    std::array<int8_t, 3> coneAxis{};
    int8_t coneCutoff = 127;
    glm::vec4 boundingSphere{0.0f}; // xyz = center, w = radius (object space)
};
static_assert(sizeof(GpuMeshletDesc) == 32, "GpuMeshletDesc must match types.slang (32 B)");
static_assert(offsetof(GpuMeshletDesc, coneAxis) == 12);
static_assert(offsetof(GpuMeshletDesc, boundingSphere) == 16);

// object-space box of one meshlet, parallel to GpuMeshletDesc (BBox in vk_lod_clusters)
struct GpuMeshletBounds
{
    glm::vec3 bboxMin{0.0f};
    // shortest and longest triangle edge, object space; for a later software-raster threshold
    float shortestEdge = 0.0f;
    glm::vec3 bboxMax{0.0f};
    float longestEdge = 0.0f;
};
static_assert(sizeof(GpuMeshletBounds) == 32, "GpuMeshletBounds must match types.slang (32 B)");
static_assert(offsetof(GpuMeshletBounds, bboxMax) == 16);

// meshlets per GpuMeshletGroup, per GpuDrawRecord and per meshlet-cull workgroup (vk_lod_clusters' group size)
inline constexpr uint32_t kMeshletGroupMaxSize = 32;
static_assert(kMeshletGroupMaxSize <= 32, "meshlet_cull reject mask is one uint per group");

// a run of a primitive's meshlets that are close in space (meshopt_partitionClusters), culled as one box before its
// meshlets are tested; the unit a cluster LOD level will be built from
struct GpuMeshletGroup
{
    glm::vec3 bboxMin{0.0f}; // object space, union of the meshlet boxes
    uint32_t firstMeshlet = 0; // relative to the primitive
    glm::vec3 bboxMax{0.0f};
    uint32_t meshletCount = 0; // <= kMeshletGroupMaxSize
};
static_assert(sizeof(GpuMeshletGroup) == 32, "GpuMeshletGroup must match types.slang (32 B)");
static_assert(offsetof(GpuMeshletGroup, bboxMax) == 16);

// one render-list entry (vk_lod_clusters ClusterInfo): the meshlet is identified by its model-relative row, which
// stays the same across draw records, phases and, later, LOD levels
struct GpuVisibleMeshlet
{
    uint32_t nodeInstanceIndex = 0;
    uint32_t meshletRow = 0; // row of the model's GpuMeshletDesc slice
};
static_assert(sizeof(GpuVisibleMeshlet) == 8);

// axis-aligned box; empty when bboxMin > bboxMax
struct BoundingBox
{
    glm::vec3 bboxMin{0.0f};
    glm::vec3 bboxMax{0.0f};
};

// Per-entity range into the global meshlet arrays.
struct MeshletDraw
{
    uint32_t firstMeshlet = 0;
    uint32_t meshletCount = 0;
    // the meshlets' triangles, stored in meshlet order: meshlet m starts (triangleOffset - first triangleOffset) / 3
    // triangles into the primitive
    uint32_t triangleCount = 0;
};

inline constexpr uint32_t kNoneIndex = ~0u;

// CPU catalog row; the shaders read the GpuPrimitive mirror
struct PrimitiveDraw
{
    MeshletDraw meshlets;
    uint32_t materialId = kNoneIndex;
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    // catalog-absolute rows of GeometryStore::meshletGroups; the groups cover the meshlets in order
    uint32_t firstGroup = 0;
    uint32_t groupCount = 0;
    // m8 I14 F27: zero-flux triangle light filter for partial textured emitters (catalog-absolute rows of
    // GeometryStore::emissiveActiveTriangles). When hasEmissiveFilter is false, every triangle of an emissive
    // primitive is active.
    uint32_t firstEmissiveTriangle = 0;
    uint32_t emissiveTriangleCount = 0;
    bool hasEmissiveFilter = false;
};

namespace NodeFlag
{
    // mirrored transform (det < 0): the mesh shader swaps triangle winding
    inline constexpr uint32_t FlipWinding = 1u << 0;
} // namespace NodeFlag

// one placement of a mesh inside its model; the mesh's primitives are stored once and shared by its nodes
struct NodeDraw
{
    glm::mat4 localToModel{1.0f};
    // model space: the mesh's sphere moved by localToModel
    glm::vec4 boundingSphere{0.0f};
    // mesh space: the box of the mesh's vertices, tested under localToModel
    BoundingBox meshBounds;
    // catalog-absolute row of the mesh's first PrimitiveDraw
    uint32_t firstPrimitive = 0;
    uint32_t primitiveCount = 0;
    uint32_t flags = 0;
};

namespace GpuMaterialFlag
{
    inline constexpr uint32_t AlphaOpaque = 0;
    inline constexpr uint32_t AlphaMask = 1;
    inline constexpr uint32_t AlphaBlend = 2;
    inline constexpr uint32_t AlphaModeMask = 3u;
    inline constexpr uint32_t DoubleSided = 1u << 2;
    inline constexpr uint32_t Unlit = 1u << 3;
    // non-identity KHR_texture_transform, one bit per GpuMaterial::uvMatrices slot
    inline constexpr uint32_t UvTransformBaseColor = 1u << 4;
    inline constexpr uint32_t UvTransformMetalRough = 1u << 5;
    inline constexpr uint32_t UvTransformNormal = 1u << 6;
    inline constexpr uint32_t UvTransformOcclusion = 1u << 7;
    inline constexpr uint32_t UvTransformEmissive = 1u << 8;
    // KHR_materials_pbrSpecularGlossiness: the base-color slot holds diffuse, the metal-rough slot specular-glossiness
    inline constexpr uint32_t SpecularGlossiness = 1u << 9;
    // packed rows only (packMaterial): smooth thin-walled glass, out of the vis buffer, on kRayMaskGlass (m8 I13)
    inline constexpr uint32_t Transmissive = 1u << 10;
} // namespace GpuMaterialFlag

inline constexpr uint32_t kMaterialUvSlotCount = 5;

// rows of M = T * R * S: uv' = M * (uv, 1), gradients take the 2x2 part
struct GpuUvMatrix
{
    glm::vec3 row0{1.0f, 0.0f, 0.0f};
    glm::vec3 row1{0.0f, 1.0f, 0.0f};

    bool operator==(const GpuUvMatrix&) const = default;
};
static_assert(sizeof(GpuUvMatrix) == 24);
static_assert(offsetof(GpuUvMatrix, row1) == 12);

struct alignas(16) GpuMaterial
{
    glm::vec4 baseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};
    glm::vec3 emissiveFactor{0.0f};
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    float normalScale = 1.0f;
    float occlusionStrength = 1.0f;
    float alphaCutoff = 0.5f;
    uint32_t flags = 0;

    uint32_t baseColorTex = kNoneIndex;
    uint32_t metalRoughTex = kNoneIndex;
    uint32_t normalTex = kNoneIndex;
    uint32_t occlusionTex = kNoneIndex;
    uint32_t emissiveTex = kNoneIndex;

    uint32_t baseColorSamp = kNoneIndex;
    uint32_t metalRoughSamp = kNoneIndex;
    uint32_t normalSamp = kNoneIndex;
    uint32_t occlusionSamp = kNoneIndex;
    uint32_t emissiveSamp = kNoneIndex;

    uint8_t baseColorUv = 0;
    uint8_t metalRoughUv = 0;
    uint8_t normalUv = 0;
    uint8_t occlusionUv = 0;
    uint8_t emissiveUv = 0;
    uint8_t pad0 = 0;
    uint8_t pad1 = 0;
    uint8_t pad2 = 0;
    // read only under GpuMaterialFlag::SpecularGlossiness
    glm::vec3 specularFactor{1.0f};

    // base color, metal-rough, normal, occlusion, emissive; valid only where the GpuMaterialFlag::UvTransform* bit is
    // set
    std::array<GpuUvMatrix, kMaterialUvSlotCount> uvMatrices{};
    // read only under GpuMaterialFlag::SpecularGlossiness
    float glossinessFactor = 1.0f;
    uint32_t pad4 = 0;

    bool operator==(const GpuMaterial&) const = default;
};
static_assert(sizeof(GpuMaterial) == 240);
static_assert(offsetof(GpuMaterial, baseColorFactor) == 0);
static_assert(offsetof(GpuMaterial, emissiveFactor) == 16);
static_assert(offsetof(GpuMaterial, metallicFactor) == 28);
static_assert(offsetof(GpuMaterial, roughnessFactor) == 32);
static_assert(offsetof(GpuMaterial, normalScale) == 36);
static_assert(offsetof(GpuMaterial, occlusionStrength) == 40);
static_assert(offsetof(GpuMaterial, alphaCutoff) == 44);
static_assert(offsetof(GpuMaterial, flags) == 48);
static_assert(offsetof(GpuMaterial, baseColorTex) == 52);
static_assert(offsetof(GpuMaterial, metalRoughTex) == 56);
static_assert(offsetof(GpuMaterial, normalTex) == 60);
static_assert(offsetof(GpuMaterial, occlusionTex) == 64);
static_assert(offsetof(GpuMaterial, emissiveTex) == 68);
static_assert(offsetof(GpuMaterial, baseColorSamp) == 72);
static_assert(offsetof(GpuMaterial, metalRoughSamp) == 76);
static_assert(offsetof(GpuMaterial, normalSamp) == 80);
static_assert(offsetof(GpuMaterial, occlusionSamp) == 84);
static_assert(offsetof(GpuMaterial, emissiveSamp) == 88);
static_assert(offsetof(GpuMaterial, baseColorUv) == 92);
static_assert(offsetof(GpuMaterial, metalRoughUv) == 93);
static_assert(offsetof(GpuMaterial, normalUv) == 94);
static_assert(offsetof(GpuMaterial, occlusionUv) == 95);
static_assert(offsetof(GpuMaterial, emissiveUv) == 96);
static_assert(offsetof(GpuMaterial, pad0) == 97);
static_assert(offsetof(GpuMaterial, pad1) == 98);
static_assert(offsetof(GpuMaterial, pad2) == 99);
static_assert(offsetof(GpuMaterial, specularFactor) == 100);
static_assert(offsetof(GpuMaterial, uvMatrices) == 112);
static_assert(offsetof(GpuMaterial, glossinessFactor) == 232);
static_assert(offsetof(GpuMaterial, pad4) == 236);

// GPU mirror of one primitive. Values are relative to the owning model's slices (GpuModel)
struct GpuPrimitive
{
    uint32_t firstMeshlet = 0;
    uint32_t meshletCount = 0;
    // absolute row of the shared material table; kNoneIndex selects the default material, row 0
    uint32_t materialId = kNoneIndex;
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    // rows of the model's GpuMeshletGroup slice
    uint32_t firstGroup = 0;
    uint32_t groupCount = 0;
    // GpuPrimitiveFlag bits
    uint32_t flags = 0;
};
static_assert(sizeof(GpuPrimitive) == 40);
static_assert(offsetof(GpuPrimitive, firstGroup) == 28);
static_assert(offsetof(GpuPrimitive, flags) == 36);

// material bits copied at load, so raster and the draw context skip the material row (materials are immutable)
namespace GpuPrimitiveFlag
{
    inline constexpr uint32_t DoubleSided = 1u << 0;
} // namespace GpuPrimitiveFlag

// GPU mirror of NodeDraw, model-relative; read by culling and ray tracing, the raster path uses GpuNodeInstance
struct GpuNode
{
    // rows of the affine localToModel
    std::array<glm::vec4, 3> localToModel{glm::vec4{1, 0, 0, 0}, glm::vec4{0, 1, 0, 0}, glm::vec4{0, 0, 1, 0}};
    glm::vec4 boundingSphere{0.0f};
    // model-relative row of the mesh's first GpuPrimitive
    uint32_t firstPrimitive = 0;
    uint32_t primitiveCount = 0;
    uint32_t flags = 0;
    uint32_t pad0 = 0;
    // mesh space; the cull passes test it under GpuNodeInstance::worldMatrix
    glm::vec3 bboxMin{0.0f};
    uint32_t pad1 = 0;
    glm::vec3 bboxMax{0.0f};
    uint32_t pad2 = 0;
};
static_assert(sizeof(GpuNode) == 112);
static_assert(offsetof(GpuNode, firstPrimitive) == 64);
static_assert(offsetof(GpuNode, bboxMin) == 80);

// one entity x node, rewritten every frame: transforms are precomputed so shaders read one matrix
struct GpuNodeInstance
{
    glm::mat4 worldMatrix{1.0f};
    // the early occlusion test places the bounds where the previous pyramid saw them
    glm::mat4 prevWorldMatrix{1.0f};
    uint32_t entityIndex = 0;
    uint32_t modelIndex = 0;
    uint32_t nodeIndex = 0; // model-relative
    uint32_t flags = 0; // NodeFlag
};
static_assert(sizeof(GpuNodeInstance) == 144);
static_assert(offsetof(GpuNodeInstance, entityIndex) == 128);

// raster pipeline a meshlet draws with (engine spec A6); one DGC execute per bin and phase. Every bin culls back
// faces: two-sided materials flip the winding of back-facing triangles in the mesh shader
enum class RasterBin : uint32_t
{
    Opaque = 0,
    // M6: discard below the alpha cutoff; empty until then
    AlphaTested,
    Count,
};
inline constexpr uint32_t kRasterBinCount = std::to_underlying(RasterBin::Count);

// resolve shader a pixel runs (engine spec §9.1, spec/m7/e1_material_binning.md): one bin per material shading
// body, plus Background for depth 0. Also the index into the resolve's DGC execution set
enum class MaterialBin : uint32_t
{
    Background = 0,
    // glTF metallic-roughness
    Standard,
    // KHR_materials_unlit
    Unlit,
    Count,
};
inline constexpr uint32_t kMaterialBinCount = std::to_underlying(MaterialBin::Count);
// edge of a classify / resolve tile in pixels; matches shaders/types.slang kMaterialTileSize
inline constexpr uint32_t kMaterialTileSize = 8;

// two-phase occlusion: early draws what passes against the previous pyramid, late what the fresh one recovers.
// each phase has its own GpuCullView, GpuCullCounters, visible list region and DGC sequences
enum class CullPhase : uint32_t
{
    Early = 0,
    Late,
    Count,
};
inline constexpr uint32_t kCullPhaseCount = std::to_underlying(CullPhase::Count);

// workgroup size of the instance cull pass and buildMeshSequences
inline constexpr uint32_t kCullGroupSize = 64;

namespace DrawRecordFlag
{
    // RasterBin in the low bits
    inline constexpr uint32_t BinMask = 0x3u;
    // double-sided material: no normal-cone test
    inline constexpr uint32_t TwoSided = 1u << 2;
} // namespace DrawRecordFlag
static_assert(kRasterBinCount - 1 <= DrawRecordFlag::BinMask);

// one meshlet cull work item: one GpuMeshletGroup of one primitive placed by one node instance.
// built once per scene layout as templates; the instance pass copies those of visible instances
struct GpuDrawRecord
{
    uint32_t nodeInstanceIndex = 0;
    uint32_t primitiveIndex = 0; // absolute row of GpuFrameRoot.primitives
    uint32_t meshletOffset = 0; // the group's first meshlet, relative to the primitive (read by the mesh shaders)
    uint32_t flags = 0; // DrawRecordFlag
    uint32_t groupOffset = 0; // the group, relative to GpuPrimitive::firstGroup
};
static_assert(sizeof(GpuDrawRecord) == 20);

// one node instance for the instance pass; its draw record templates are contiguous
struct GpuInstanceCullItem
{
    uint32_t nodeInstanceIndex = 0;
    uint32_t firstDrawRecord = 0; // row of GpuCullView::drawRecordTemplates
    uint32_t drawRecordCount = 0;
};
static_assert(sizeof(GpuInstanceCullItem) == 12);

namespace CullFlag
{
    // meshlet sphere against the frustum
    inline constexpr uint32_t Frustum = 1u << 0;
    // normal cone backface test, single-sided group only
    inline constexpr uint32_t Cone = 1u << 1;
    inline constexpr uint32_t Stats = 1u << 2;
    // node instance sphere against the frustum
    inline constexpr uint32_t InstanceFrustum = 1u << 3;
    // hi-z test of instances and meshlets against GpuCullView::occlusionViewProj (shaders/core/culling.slang)
    inline constexpr uint32_t Occlusion = 1u << 4;
    // set on the late view: instances come from the candidate list and skip the frustum test; meshlets are tested
    // with current transforms
    inline constexpr uint32_t LatePhase = 1u << 5;
    // instances and meshlets whose screen rect holds no pixel center (shaders/core/culling.slang isSubPixel)
    inline constexpr uint32_t SubPixel = 1u << 6;
} // namespace CullFlag

// GpuFrameRoot::triangleCull: per-triangle tests in the mesh shaders (shaders/geometry/triangle_cull.slang)
namespace TriangleCullFlag
{
    // signed screen area; the double-sided group skips it
    inline constexpr uint32_t Backface = 1u << 0;
    // bounding box misses every pixel center
    inline constexpr uint32_t Small = 1u << 1;
    // triangle counters in GpuCullCounters
    inline constexpr uint32_t Stats = 1u << 2;
} // namespace TriangleCullFlag

// per frame slot, host-written: what the cull passes test against and where they read and write
struct GpuCullView
{
    glm::vec3 cameraPos{0.0f};
    uint32_t flags = 0; // CullFlag
    // GpuVisibleMeshlet[], one region per RasterBin
    vk::DeviceAddress visibleMeshlets = 0;
    vk::DeviceAddress counters = 0; // GpuCullCounters
    vk::DeviceAddress sequences = 0; // MeshDgcSequence[], one region per RasterBin
    vk::DeviceAddress instanceItems = 0; // GpuInstanceCullItem[instanceCount]
    vk::DeviceAddress drawRecordTemplates = 0; // GpuDrawRecord[], copied to GpuFrameRoot::drawRecords
    uint32_t instanceCount = 0;
    uint32_t maxMeshletsPerSequence = 0;
    std::array<uint32_t, kRasterBinCount> binVisibleOffset{};
    std::array<uint32_t, kRasterBinCount> binSequenceOffset{};
    // early: previous frame's viewProj against the pyramid it built; late: current viewProj and fresh pyramid
    glm::mat4 occlusionViewProj{1.0f};
    uint32_t hizPyramid = 0; // resource heap, full mip chain
    uint32_t hizFarSampler = 0; // sampler heap: far-depth reduction
    vk::Extent2D hizSize{}; // mip 0
    // uint[]: GpuInstanceCullItem rows occluded in the early phase, re-tested by the late one
    vk::DeviceAddress instanceCandidates = 0;
    // GpuCullCounters whose instanceDispatch counts the candidates (the late phase's); early view only
    vk::DeviceAddress lateCounters = 0;
    // first row of GpuFrameRoot::drawRecords this phase appends to
    uint32_t drawRecordBase = 0;
    // first row of the early phase's draw records; rejectMasks is indexed relative to it
    uint32_t rejectRecordBase = 0;
    // uint per early draw record: bit i = meshlet i of its group failed only occlusion; written early, read late
    vk::DeviceAddress rejectMasks = 0;
    // frustum and sub-pixel tests: current viewProj, or the frozen cull camera's
    glm::mat4 cullViewProj{1.0f};
    glm::vec2 targetSize{0.0f}; // render target in pixels
    // pixels the raster may move a corner against cullViewProj: the subpixel grid snap (subPixelPrecisionBits) plus
    // this frame's jitter, which the cull matrix leaves out (engine spec §12.2)
    float subpixelSnap = 0.0f;
    // capacities of the lists this phase appends to; appends past them are dropped and counted in GpuCullCounters
    uint32_t instanceCandidateCapacity = 0;
    uint32_t drawRecordCapacity = 0; // rows of this phase's drawRecords region, also the reject masks
    std::array<uint32_t, kRasterBinCount> binVisibleCapacity{};
    uint32_t pad1 = 0;
};
static_assert(sizeof(GpuCullView) == 288);
static_assert(offsetof(GpuCullView, instanceCandidateCapacity) == 268);
static_assert(offsetof(GpuCullView, binVisibleCapacity) == 276);
static_assert(offsetof(GpuCullView, cullViewProj) == 192);
static_assert(offsetof(GpuCullView, rejectMasks) == 184);
static_assert(offsetof(GpuCullView, instanceCandidates) == 160);
static_assert(offsetof(GpuCullView, occlusionViewProj) == 80);
static_assert(offsetof(GpuCullView, visibleMeshlets) == 16);
static_assert(offsetof(GpuCullView, instanceItems) == 40);
static_assert(offsetof(GpuCullView, binVisibleOffset) == 64);

// one per CullPhase, back to back per frame slot; reset to kInitialCullCounters before the early cull
struct GpuCullCounters
{
    std::array<uint32_t, kRasterBinCount> visibleMeshlets{};
    // DGC sequenceCountAddress of each group's execute
    std::array<uint32_t, kRasterBinCount> sequenceCount{};
    // VkDispatchIndirectCommand of the meshlet cull: x = draw records of visible instances
    std::array<uint32_t, 3> meshletDispatch{0, 1, 1};
    // VkDispatchIndirectCommand of the late instance cull: x = candidates; late phase only
    std::array<uint32_t, 3> instanceDispatch{0, 1, 1};
    // late phase, CullFlag::Stats only: reject-mask bits re-tested against the fresh pyramid
    uint32_t meshletsRetested = 0;
    // CullFlag::Stats only
    uint32_t instancesCulled = 0; // frustum
    uint32_t instancesDrawn = 0;
    uint32_t instancesOccluded = 0;
    uint32_t testedMeshlets = 0;
    uint32_t frustumCulled = 0;
    uint32_t coneCulled = 0;
    // early: set in the reject masks; late: still occluded
    uint32_t occlusionCulled = 0;
    // mesh shader, TriangleCullFlag::Stats only
    uint32_t trianglesTested = 0;
    uint32_t trianglesBackface = 0;
    uint32_t trianglesSmall = 0;
    // CullFlag::Stats only
    uint32_t instancesSubPixel = 0;
    uint32_t meshletsSubPixel = 0;
    // meshlet groups (draw records) whose box was tested, and those culled whole by it; CullFlag::Stats only
    uint32_t groupsTested = 0;
    uint32_t groupsCulled = 0;
    // always counted: appends dropped at capacity; nonzero means a list is sized too small
    uint32_t candidatesOverflowed = 0;
    uint32_t drawRecordsOverflowed = 0;
    uint32_t visibleOverflowed = 0;
};
// Debug builds (kDebugRays): what the trace helpers report; zeroed by the CPU per frame
struct GpuRayDebugCounters
{
    // queries still running after their one Proceed(): candidates only an any-hit or a candidate loop resolves
    uint32_t candidates = 0;
};
static_assert(sizeof(GpuRayDebugCounters) == 4);

static_assert(sizeof(GpuCullCounters) == 112);
static_assert(offsetof(GpuCullCounters, candidatesOverflowed) == 100);
static_assert(offsetof(GpuCullCounters, meshletsRetested) == 40);
static_assert(offsetof(GpuCullCounters, instanceDispatch) == 28);
static_assert(offsetof(GpuCullCounters, sequenceCount) == 8);
static_assert(offsetof(GpuCullCounters, meshletDispatch) == 16);
inline constexpr std::array<GpuCullCounters, kCullPhaseCount> kInitialCullCounters{};

// CPU authoring form of KHR PBR-next + texture-transform; uploaded packed as GpuMaterialExtension
struct MaterialUvTransform
{
    glm::vec2 offset{0.0f, 0.0f};
    glm::vec2 scale{1.0f, 1.0f};
    float rotation = 0.0f;

    bool operator==(const MaterialUvTransform&) const = default;
};

struct MaterialTextureRef
{
    uint32_t tex = kNoneIndex;
    uint32_t samp = kNoneIndex;
    uint8_t uv = 0;
    float normalScale = 1.0f;
    MaterialUvTransform uvXform{};

    bool operator==(const MaterialTextureRef&) const = default;
};

namespace MaterialExtFlag
{
    inline constexpr uint32_t Unlit = 1u << 0;
    inline constexpr uint32_t Clearcoat = 1u << 1;
    inline constexpr uint32_t Sheen = 1u << 2;
    inline constexpr uint32_t Transmission = 1u << 3;
    inline constexpr uint32_t Volume = 1u << 4;
    inline constexpr uint32_t Ior = 1u << 5;
    inline constexpr uint32_t Specular = 1u << 6;
    inline constexpr uint32_t Iridescence = 1u << 7;
    inline constexpr uint32_t Anisotropy = 1u << 8;
    inline constexpr uint32_t EmissiveStrength = 1u << 9;
    inline constexpr uint32_t Dispersion = 1u << 10;
    inline constexpr uint32_t DiffuseTransmission = 1u << 11;
} // namespace MaterialExtFlag

struct MaterialPbrExtension
{
    uint32_t flags = 0;

    MaterialUvTransform baseColorUv{};
    MaterialUvTransform metalRoughUv{};
    MaterialUvTransform normalUv{};
    MaterialUvTransform occlusionUv{};
    MaterialUvTransform emissiveUv{};

    float ior = 1.5f;
    float emissiveStrength = 1.0f;
    float dispersion = 0.0f;

    float specularFactor = 1.0f;
    glm::vec3 specularColorFactor{1.0f};
    MaterialTextureRef specular;
    MaterialTextureRef specularColor;

    float clearcoatFactor = 0.0f;
    float clearcoatRoughnessFactor = 0.0f;
    MaterialTextureRef clearcoat;
    MaterialTextureRef clearcoatRoughness;
    MaterialTextureRef clearcoatNormal;

    glm::vec3 sheenColorFactor{0.0f};
    float sheenRoughnessFactor = 0.0f;
    MaterialTextureRef sheenColor;
    MaterialTextureRef sheenRoughness;

    float transmissionFactor = 0.0f;
    MaterialTextureRef transmission;

    float thicknessFactor = 0.0f;
    float attenuationDistance = 0.0f; // 0 = infinite
    glm::vec3 attenuationColor{1.0f};
    MaterialTextureRef thickness;

    float iridescenceFactor = 0.0f;
    float iridescenceIor = 1.3f;
    float iridescenceThicknessMin = 100.0f;
    float iridescenceThicknessMax = 400.0f;
    MaterialTextureRef iridescence;
    MaterialTextureRef iridescenceThickness;

    float anisotropyStrength = 0.0f;
    float anisotropyRotation = 0.0f;
    MaterialTextureRef anisotropy;

    float diffuseTransmissionFactor = 0.0f;
    glm::vec3 diffuseTransmissionColorFactor{1.0f};
    MaterialTextureRef diffuseTransmission;
    MaterialTextureRef diffuseTransmissionColor;

    bool operator==(const MaterialPbrExtension&) const = default;
};

// GPU mirrors of the above: 4-byte fields only, no implicit padding (scalar layout, types.slang)
struct GpuUvTransform
{
    glm::vec2 offset{0.0f, 0.0f};
    glm::vec2 scale{1.0f, 1.0f};
    float rotation = 0.0f;
};
static_assert(sizeof(GpuUvTransform) == 20);

struct GpuTextureRef
{
    uint32_t tex = kNoneIndex;
    uint32_t samp = kNoneIndex;
    uint32_t uv = 0;
    float normalScale = 1.0f;
    GpuUvTransform uvXform{};
};
static_assert(sizeof(GpuTextureRef) == 36);

// one row per GpuMaterial row, same index
struct GpuMaterialExtension
{
    uint32_t flags = 0;

    GpuUvTransform baseColorUv{};
    GpuUvTransform metalRoughUv{};
    GpuUvTransform normalUv{};
    GpuUvTransform occlusionUv{};
    GpuUvTransform emissiveUv{};

    float ior = 1.5f;
    float emissiveStrength = 1.0f;
    float dispersion = 0.0f;

    float specularFactor = 1.0f;
    glm::vec3 specularColorFactor{1.0f};
    GpuTextureRef specular;
    GpuTextureRef specularColor;

    float clearcoatFactor = 0.0f;
    float clearcoatRoughnessFactor = 0.0f;
    GpuTextureRef clearcoat;
    GpuTextureRef clearcoatRoughness;
    GpuTextureRef clearcoatNormal;

    glm::vec3 sheenColorFactor{0.0f};
    float sheenRoughnessFactor = 0.0f;
    GpuTextureRef sheenColor;
    GpuTextureRef sheenRoughness;

    float transmissionFactor = 0.0f;
    GpuTextureRef transmission;

    float thicknessFactor = 0.0f;
    float attenuationDistance = 0.0f;
    glm::vec3 attenuationColor{1.0f};
    GpuTextureRef thickness;

    float iridescenceFactor = 0.0f;
    float iridescenceIor = 1.3f;
    float iridescenceThicknessMin = 100.0f;
    float iridescenceThicknessMax = 400.0f;
    GpuTextureRef iridescence;
    GpuTextureRef iridescenceThickness;

    float anisotropyStrength = 0.0f;
    float anisotropyRotation = 0.0f;
    GpuTextureRef anisotropy;

    float diffuseTransmissionFactor = 0.0f;
    glm::vec3 diffuseTransmissionColorFactor{1.0f};
    GpuTextureRef diffuseTransmission;
    GpuTextureRef diffuseTransmissionColor;
};
static_assert(sizeof(GpuMaterialExtension) == 724);
static_assert(offsetof(GpuMaterialExtension, ior) == 104);
static_assert(offsetof(GpuMaterialExtension, specular) == 132);
static_assert(offsetof(GpuMaterialExtension, clearcoat) == 212);
static_assert(offsetof(GpuMaterialExtension, sheenColor) == 336);
static_assert(offsetof(GpuMaterialExtension, thickness) == 468);
static_assert(offsetof(GpuMaterialExtension, iridescence) == 520);
static_assert(offsetof(GpuMaterialExtension, diffuseTransmission) == 652);

// GpuLight::type, matches LightType in shaders/types.slang
enum class GpuLightType : uint32_t
{
    Directional = 0,
    Point = 1,
    Spot = 2,
    // world rows only (GpuTriangleLight), written by light_world from an emissive primitive's triangles
    Triangle = 3,
};

struct LightDef
{
    GpuLightType type = GpuLightType::Point;
    glm::vec3 color{1.0f};
    float intensity = 1.0f;
    float range = 0.0f;
    float innerCone = 0.0f;
    float outerCone = 0.785398163f;
};

// model space: the same space as the model's baked vertices
struct LightInstance
{
    uint32_t defIndex = kNoneIndex;
    glm::vec3 position{0.0f};
    glm::vec3 direction{0.0f, 0.0f, -1.0f};
};

// folded LightDef + LightInstance for the light SSBO (RT/PT later).
// model space; shaders move it to world space with instances[instanceIndex].modelMatrix
struct alignas(16) GpuLight
{
    glm::vec3 position{0.0f};
    float range = 0.0f;
    glm::vec3 direction{0.0f, 0.0f, -1.0f};
    float intensity = 1.0f;
    glm::vec3 color{1.0f};
    GpuLightType type = GpuLightType::Point;
    float innerCone = 0.0f;
    float outerCone = 0.785398163f;
    // owning entity, absolute: shifts down on unload like every entity id
    uint32_t instanceIndex = 0;
    float pad0 = 0.0f;
};
static_assert(sizeof(GpuLight) == 64, "GpuLight must stay 64 B");
static_assert(offsetof(GpuLight, position) == 0);
static_assert(offsetof(GpuLight, range) == 12);
static_assert(offsetof(GpuLight, direction) == 16);
static_assert(offsetof(GpuLight, intensity) == 28);
static_assert(offsetof(GpuLight, color) == 32);
static_assert(offsetof(GpuLight, type) == 44);
static_assert(offsetof(GpuLight, innerCone) == 48);
static_assert(offsetof(GpuLight, outerCone) == 52);
static_assert(offsetof(GpuLight, instanceIndex) == 56);
static_assert(offsetof(GpuLight, pad0) == 60);

// a world light row of type Triangle (spec/m7/e2_emissive_triangles.md), the GpuLight row reinterpreted: type at the
// same offset. One emitting triangle, radiance constant over it
struct GpuTriangleLight
{
    glm::vec3 v0{0.0f};
    glm::vec3 edge1{0.0f};
    glm::vec3 edge2{0.0f};
    // GpuTriangleLightFlag bits
    uint32_t flags = 0;
    uint32_t pad0 = 0;
    GpuLightType type = GpuLightType::Triangle;
    // emitted radiance, W / (sr m^2) in scene units (multi-tap average in light_world)
    glm::vec3 radiance{0.0f};
    uint32_t pad1 = 0;
};
static_assert(sizeof(GpuTriangleLight) == sizeof(GpuLight));
static_assert(offsetof(GpuTriangleLight, edge1) == 12);
static_assert(offsetof(GpuTriangleLight, edge2) == 24);
static_assert(offsetof(GpuTriangleLight, flags) == 36);
static_assert(offsetof(GpuTriangleLight, type) == offsetof(GpuLight, type));
static_assert(offsetof(GpuTriangleLight, radiance) == 48);

namespace GpuTriangleLightFlag
{
    // the material is double-sided: both faces emit
    inline constexpr uint32_t DoubleSided = 1u << 0;
    // the placement mirrors: cross(edge1, edge2) points out of the back face
    inline constexpr uint32_t Mirrored = 1u << 1;
} // namespace GpuTriangleLightFlag

namespace AuxOwnerKind
{
    inline constexpr uint32_t Model = 0;
    inline constexpr uint32_t Asset = 1;
    inline constexpr uint32_t Mesh = 2;
    inline constexpr uint32_t Primitive = 3;
    inline constexpr uint32_t Material = 4;
    inline constexpr uint32_t Sampler = 5;
    inline constexpr uint32_t Texture = 6;
    inline constexpr uint32_t Image = 7;
    inline constexpr uint32_t Light = 8;
} // namespace AuxOwnerKind

struct AuxBlob
{
    uint32_t ownerKind = 0;
    uint32_t ownerIndex = 0;
    std::string extrasJson;
    std::vector<std::pair<std::string, std::string>> extensions;
};


struct alignas(16) GpuCameraData
{
    // Primary Matrices
    glm::mat4 view;
    glm::mat4 proj;
    glm::mat4 viewProj;

    // Inverses (for ray tracing, deferred depth reconstruction, world-space position calculation)
    glm::mat4 invView;
    glm::mat4 invProj;
    glm::mat4 invViewProj;

    // Temporal (for Motion Vectors / TAA / Velocity Buffers)
    glm::mat4 prevViewProj;

    // Position & View Parameters
    glm::vec3 cameraPos; // World-space camera position (for specular/lighting calculations)
    float nearZ; // Near clipping plane

    glm::vec2 renderTargetSize; // Width, Height in pixels
    glm::vec2 invRenderTargetSize; // 1.0 / Width, 1.0 / Height

    glm::vec2 jitterOffset; // TAA subpixel jitter offset
    float farZ; // Far clipping plane
    float frameDeltaTime; // Delta time in seconds
    // cameraPos of the previous frame; equal to cameraPos on a cut, like prevViewProj
    glm::vec3 prevCameraPos;
    // texture LOD bias log2(render / display) under DLSS (engine spec §12.2); applied by scaling the gradients
    float lodBias;

    // world-space ray basis at unit view depth for jittered pixel centers: rayDirScaled = rayOrigin + pixel.x * rayDx
    // + pixel.y * rayDy, worldPos = cameraPos + rayDirScaled * viewDepth (m8 O4)
    glm::vec3 rayOrigin;
    glm::vec3 rayDx;
    glm::vec3 rayDy;
    // previous frame's ray basis (exact previous invView, proj and jitter); equals current on a cut
    glm::vec3 prevRayOrigin;
    glm::vec3 prevRayDx;
    glm::vec3 prevRayDy;
    float rayPad0;
    float rayPad1;
};
static_assert(sizeof(GpuCameraData) == 592);
static_assert(offsetof(GpuCameraData, prevViewProj) == 384);
static_assert(offsetof(GpuCameraData, cameraPos) == 448);
static_assert(offsetof(GpuCameraData, renderTargetSize) == 464);
static_assert(offsetof(GpuCameraData, prevCameraPos) == 496);
static_assert(offsetof(GpuCameraData, rayOrigin) == 512);
static_assert(offsetof(GpuCameraData, prevRayOrigin) == 548);

// one row per entity in the per-frame instance buffer
struct alignas(16) GpuObjectUB
{
    glm::mat4 modelMatrix;
    // previous frame, for motion vectors
    glm::mat4 prevModelMatrix;
    // world space: xyz = center, w = radius
    glm::vec4 boundingSphere;
    // row in GpuFrameRoot::models
    uint32_t modelIndex;
    uint32_t instanceFlags;
    uint32_t pad0;
    uint32_t pad1;
};
static_assert(sizeof(GpuObjectUB) == 160);
static_assert(offsetof(GpuObjectUB, modelIndex) == 144);

// GpuModel::attributeMask bits
namespace GpuModelAttribute
{
    inline constexpr uint32_t Normals = 1u << 0;
    inline constexpr uint32_t Tangents = 1u << 1;
    inline constexpr uint32_t Uv1 = 1u << 2;
    inline constexpr uint32_t Indices = 1u << 3;
} // namespace GpuModelAttribute

// slice addresses of one loaded model; data inside a slice is model-relative. 0 = stream absent
struct GpuModel
{
    vk::DeviceAddress vertices = 0;
    vk::DeviceAddress meshlets = 0;
    vk::DeviceAddress meshletBounds = 0; // GpuMeshletBounds[], parallel to meshlets
    vk::DeviceAddress meshletGroups = 0; // GpuMeshletGroup[]
    vk::DeviceAddress meshletPrimitives = 0; // uint per meshlet: its row of the primitives slice
    vk::DeviceAddress meshletVertices = 0;
    vk::DeviceAddress meshletTriangles = 0;
    vk::DeviceAddress normals = 0;
    vk::DeviceAddress tangents = 0;
    vk::DeviceAddress uv1 = 0;
    vk::DeviceAddress indices = 0;
    vk::DeviceAddress nodes = 0;
    vk::DeviceAddress primitives = 0; // GpuPrimitive[], the model's slice of GpuFrameRoot::primitives
    uint32_t attributeMask = 0;
    uint32_t nodeCount = 0;
};
static_assert(sizeof(GpuModel) == 112);
static_assert(offsetof(GpuModel, meshletBounds) == 16);
static_assert(offsetof(GpuModel, meshletGroups) == 24);
static_assert(offsetof(GpuModel, meshletPrimitives) == 32);
static_assert(offsetof(GpuModel, primitives) == 96);
static_assert(offsetof(GpuModel, attributeMask) == 104);

// vis-buffer payload: (visible row << kVisTriangleBits) | meshlet-local triangle (shaders/geometry/visbuffer.slang)
inline constexpr uint32_t kVisTriangleBits = 7;
inline constexpr uint32_t kMaxVisibleRows = 1u << (32 - kVisTriangleBits);

// GpuGuideSet rows (docs/engine_spec.md §9.3); must match shaders/types.slang kGuide*
enum class GuideImage : uint32_t
{
    // RGBA16F: world shading normal, perceptual roughness (DLSS-RR)
    NormalRoughness = 0,
    // RGBA8: baseColor * (1 - metallic), a = ao (DLSS-RR, RTXDI, combine)
    DiffuseAlbedo,
    // RGBA8: environment BRDF approximation (DLSS-RR)
    SpecAlbedo,
    // R11G11B10F (combine, NRC)
    Emissive,
    // RGBA16F: render-resolution pixels to the previous frame, no jitter; z = view depth there minus here
    Motion,
    // RGBA32_UINT: packed RTXDI surface guide (x = asuint(viewDepth), y = packOct16(n), z = packOct16(geoNormal),
    // w = f0.rgb unorm8x3 | (isLit ? 1 + round(roughness * 254) : 0) << 24; m8 O4)
    Surface,
    Count,
};
inline constexpr size_t kGuideImageCount = std::to_underlying(GuideImage::Count);
// temporal passes read the previous frame's set
inline constexpr uint32_t kGuideSetCount = 2;

// heap indices of one guide set; storage views are written by the resolve, sampled views read by later passes
struct GpuGuideSet
{
    std::array<uint32_t, kGuideImageCount> storage{};
    std::array<uint32_t, kGuideImageCount> sampled{};
};
static_assert(sizeof(GpuGuideSet) == 48);

// passes/exposure.slang's log2-luminance histogram, including black in bin 0
inline constexpr uint32_t kExposureHistogramBins = 256;
inline constexpr float kExposureMinLog2 = -12.0f;
inline constexpr float kExposureLog2Range = 24.0f;

// written by passes/exposure.slang each frame it runs; read by the CPU after the slot's fence
struct GpuExposureReadback
{
    // scale applied before the tonemap
    float exposure = 0.0f;
    // where the adaptation is heading
    float targetExposure = 0.0f;
    // the histogram's trimmed geometric mean; 0 when no pixel was metered
    float averageLuminance = 0.0f;
    // finite pixels in the histogram
    uint32_t pixels = 0;
};
static_assert(sizeof(GpuExposureReadback) == 16);
static_assert(offsetof(GpuExposureReadback, pixels) == 12);

// render-resolution targets of one frame, reached from GpuFrameRoot::targets
struct GpuTargets
{
    // sampled R32_UINT, eShaderReadOnlyOptimal after the late raster
    uint32_t visBuffer = 0;
    // sampled D32_SFLOAT, reverse Z
    uint32_t depth = 0;
    // RGBA16F written by passes/debug_view.slang, sampled by passes/present.slang
    uint32_t displayStorage = 0;
    uint32_t displaySampled = 0;
    // sampler heap: linear, clamp to edge
    uint32_t linearSampler = 0;
    uint32_t pad0 = 0;
    glm::uvec2 renderSize{0};
    // this frame's set, by frame parity
    GpuGuideSet current;
    GpuGuideSet previous;
    // RGBA16F linear HDR written by passes/rtxdi_shade.slang; no emissive
    uint32_t directRadianceStorage = 0;
    uint32_t directRadianceSampled = 0;
    // RGBA16F linear HDR written by passes/rtxdi_gi_shade.slang: ReSTIR GI's one-bounce + cache term
    uint32_t indirectRadianceStorage = 0;
    uint32_t indirectRadianceSampled = 0;
    // RGBA16F linear HDR written by passes/combine.slang: direct + emissive, the DLSS-RR color input
    uint32_t noisyColorStorage = 0;
    uint32_t noisyColorSampled = 0;
    // sampled by passes/present.slang: the post target (display resolution) for the Lit view, else the display image
    // (render resolution)
    uint32_t presentSampled = 0;
    // RGBA16F linear HDR DLSS-RR output, read by passes/exposure.slang and passes/post.slang
    uint32_t upscaledSampled = 0;
    // RGBA16F display resolution, written by passes/post.slang: tonemapped, linear [0, 1]
    uint32_t postStorage = 0;
    // R32F 1x1: the exposure scale for adaptation and tonemapping
    uint32_t exposureStorage = 0;
    glm::uvec2 displaySize{0};
    // R8_UINT MaterialBin per pixel, written by passes/classify.slang
    uint32_t binIdsStorage = 0;
    uint32_t binIdsSampled = 0;
    // R32F smoothed ReSTIR GI stagnancy by frame parity: this frame's written by passes/rtxdi_gi_shade.slang, last
    // frame's sampled (m8 O11)
    uint32_t giStagnancyStorage = 0;
    uint32_t giStagnancyPrevious = 0;
    // R32_UINT final ReSTIR GI sample per pixel (0: none), written by passes/rtxdi_gi_shade.slang; R32F how many of
    // its 17x17 neighbors share it, written by passes/rtxdi_gi_duplication.slang and read by next frame's temporal
    // pass (m8 O11)
    uint32_t giSampleIdsStorage = 0;
    uint32_t giDuplicationStorage = 0;
    // RGBA8 DLSS-RR albedo guides blended with the glass layers, written by passes/glass.slang under
    // GlassFlag::BlendGuides (m8 I13 §4 variant b)
    uint32_t glassDiffuseAlbedoStorage = 0;
    uint32_t glassSpecAlbedoStorage = 0;
    // R16F DLSS-RR specular hit distance (indirect, rtxdi_gi_shade, glass; kNoneIndex when unused, m8 I16 phase 3)
    uint32_t specularHitDistanceStorage = kNoneIndex;
    // RGBA32F sum and count of the DLSS-RR color input, written by passes/combine.slang (settings lab, m8 I16)
    uint32_t dlssInputAccumulationStorage = kNoneIndex;
};
static_assert(sizeof(GpuTargets) == 216);
static_assert(offsetof(GpuTargets, specularHitDistanceStorage) == 208);
static_assert(offsetof(GpuTargets, glassDiffuseAlbedoStorage) == 200);
static_assert(offsetof(GpuTargets, giStagnancyStorage) == 184);
static_assert(offsetof(GpuTargets, binIdsStorage) == 176);
static_assert(offsetof(GpuTargets, current) == 32);

// GpuFrameRoot::glassFlags (m8 I13), matches shaders/types.slang kGlassFlag*
namespace GlassFlag
{
    // the glass pass composites the layers in front of the vis-buffer surface
    inline constexpr uint32_t Pass = 1u << 0;
    // final DI, X2's direct light and final ReSTIR GI shading multiply the glass transmittance of their segment
    inline constexpr uint32_t ShadowTransmittance = 1u << 1;
    // the layer terms of the composite: Fresnel reflection, the diffuse share, the glass's emission
    inline constexpr uint32_t Reflection = 1u << 2;
    inline constexpr uint32_t DiffuseShare = 1u << 3;
    inline constexpr uint32_t Emission = 1u << 4;
    // DLSS-RR albedo guides blended with the layers (I13 §4 variant b); off: the surface behind (variant a)
    inline constexpr uint32_t BlendGuides = 1u << 5;
} // namespace GlassFlag

// the most layers a glass walk crosses (GlassSettings::maxLayers)
inline constexpr uint32_t kGlassMaxLayersLimit = 8;

// glass pass counters of one frame slot, reset by the host before the slot's submit (m8 I13 §6)
struct GpuGlassCounters
{
    // pixels with at least one glass layer in front of their surface
    uint32_t pixels = 0;
    // walks stopped at luminance(throughput) < kGlassMinThroughput
    uint32_t earlyStops = 0;
    // walks that reached glassMaxLayers with throughput left
    uint32_t capped = 0;
    uint32_t pad0 = 0;
    // [n]: pixels whose walk crossed n + 1 layers
    std::array<uint32_t, kGlassMaxLayersLimit> layerPixels{};
};
static_assert(sizeof(GpuGlassCounters) == 48);
static_assert(offsetof(GpuGlassCounters, layerPixels) == 16);

// GpuFrameRoot::frameFlags, matches shaders/types.slang kFrameFlag*
namespace FrameFlag
{
    // last frame's images and rows do not apply: motion is zero, temporal passes skip reuse
    inline constexpr uint32_t HistoryReset = 1u << 0;
    // the indirect mode or a ReSTIR GI reference toggle changed: ReSTIR GI skips temporal reuse
    inline constexpr uint32_t IndirectHistoryReset = 1u << 1;
    // WorldLights::areTriangleLightsSampled off: RTXDI's local region holds the point and spot rows only, BRDF rays
    // name no light and hits on light triangles add their emission
    inline constexpr uint32_t TriangleLightsUnsampled = 1u << 2;
    // ENGINE_DLSS_CHECK (m8 I14 F25): synthetic clean radiance and guide perturbation controls
    inline constexpr uint32_t CleanDlssInput = 1u << 3;
    inline constexpr uint32_t InvertDlssNormal = 1u << 4;
    inline constexpr uint32_t SquareDlssRoughness = 1u << 5;
    // settings lab attribution (m8 I16): the DLSS-RR color input keeps one lighting term (plus emission)
    inline constexpr uint32_t DlssInputDirectOnly = 1u << 6;
    inline constexpr uint32_t DlssInputIndirectOnly = 1u << 7;
    // the DLSS-RR color input is the running mean of this phase's frames (restarted by HistoryReset), or that mean
    // held still: what DLSS-RR does with a converged, unchanging input
    inline constexpr uint32_t DlssInputAccumulate = 1u << 8;
    inline constexpr uint32_t DlssInputFrozen = 1u << 9;
    // GpuFrameRoot::preDenoiserClamp's reference: the 3x3 neighbors of the noisy input instead of DLSS-RR's last output
    inline constexpr uint32_t PreDenoiserClampNeighbors = 1u << 10;
    // last frame evaluated DLSS-RR into upscaledTarget and history was not reset
    inline constexpr uint32_t DlssHistoryValid = 1u << 11;
} // namespace FrameFlag

// [InstanceIndex()] of a TLAS instance; written by the CPU per scene layout (RayTracingScene)
struct GpuRayInstance
{
    // row in GpuFrameRoot::nodeInstances
    uint32_t nodeInstanceIndex = 0;
    // row in GpuFrameRoot::primitives
    uint32_t primitiveRow = 0;
};
static_assert(sizeof(GpuRayInstance) == 8);
static_assert(offsetof(GpuRayInstance, nodeInstanceIndex) == 0);

// per ray instance and scene layout, parallel to GpuRayInstance: what the PTLAS instance write needs beyond the
// node instance (RayTracingScene)
struct GpuRayInstanceBuild
{
    vk::DeviceAddress blas = 0;
    // grid partition, fixed per layout
    uint32_t partition = 0;
    // GpuRayInstanceBuildFlag bits
    uint32_t flags = 0;
};
static_assert(sizeof(GpuRayInstanceBuild) == 16);
static_assert(offsetof(GpuRayInstanceBuild, partition) == 8);
static_assert(offsetof(GpuRayInstanceBuild, flags) == 12);

namespace GpuRayInstanceBuildFlag
{
    // the primitive's clusters carry a micromap: the instance drops FORCE_OPAQUE
    inline constexpr uint32_t OpacityMicromap = 1u << 0;
    // thin-walled glass (m8 I13): the instance goes on kRayMaskGlass instead of kRayMaskOpaque
    inline constexpr uint32_t Glass = 1u << 1;
} // namespace GpuRayInstanceBuildFlag
static_assert(offsetof(GpuRayInstance, primitiveRow) == 4);

// high bit of a rayInstanceLights[rayInstance] entry: the entry is an offset into the sparse remap tail of
// rayInstanceLights rather than a dense firstLight (m8 I14 F27 zero-flux triangle light culling)
inline constexpr uint32_t kRayInstanceLightSparseBit = 0x80000000u;

// one active triangle light (m8 I14 F27): direct O(1) lookup for light_world after zero-flux culling
struct GpuTriangleLightRef
{
    // row of GpuFrameRoot::rayInstances
    uint32_t rayInstance = 0;
    // triangle index in the primitive's meshlet order
    uint32_t localTriangle = 0;
};
static_assert(sizeof(GpuTriangleLightRef) == 8);
static_assert(offsetof(GpuTriangleLightRef, localTriangle) == 4);

// per-frame entry point for every shader: the only address the push data carries
struct GpuFrameRoot
{
    vk::DeviceAddress camera = 0;
    // GpuObjectUB[entity]
    vk::DeviceAddress instances = 0;
    // GpuModel[model]
    vk::DeviceAddress models = 0;
    // GpuPrimitive[], absolute rows
    vk::DeviceAddress primitives = 0;
    // whole GpuMaterial[] buffer, shared by all models; GpuPrimitive::materialId indexes it
    vk::DeviceAddress materials = 0;
    // whole GpuMaterialExtension[] buffer, parallel to materials
    vk::DeviceAddress pbrExtensions = 0;
    // GpuLight[lightCount] in model space, point and spot before directional (AssetBuffers::uploadLights)
    vk::DeviceAddress lights = 0;
    // raw acceleration structure address, 0 until RT exists
    vk::DeviceAddress tlas = 0;
    // GpuNodeInstance[nodeInstanceCount] of this frame slot
    vk::DeviceAddress nodeInstances = 0;
    // GpuDrawRecord[] of this frame slot, written by the instance cull pass (count in GpuCullCounters)
    vk::DeviceAddress drawRecords = 0;
    // GpuVisibleMeshlet[]: GpuCullView::visibleMeshlets of this frame slot; MeshPushData::firstVisible indexes it
    vk::DeviceAddress visibleMeshlets = 0;
    uint32_t lightCount = 0;
    uint32_t instanceCount = 0;
    uint32_t primitiveCount = 0;
    uint32_t nodeInstanceCount = 0;
    // first visibleMeshlets row of the late phase
    uint32_t lateVisibleBase = 0;
    // DebugViewSettings::packed()
    uint32_t debugView = 0;
    // GpuCullCounters[kCullPhaseCount] of this frame slot; the mesh shaders add their triangle counts
    vk::DeviceAddress cullCounters = 0;
    // TriangleCullFlag
    uint32_t triangleCull = 0;
    // pixels a vertex may move when snapped to the subpixel grid (subPixelPrecisionBits)
    float subpixelSnap = 0.0f;
    // GpuTargets of this frame slot
    vk::DeviceAddress targets = 0;
    // FrameFlag
    uint32_t frameFlags = 0;
    // RTXDI's local region: world rows [0, localLightCount) are point and spot, then the triangle lights; the rest
    // are directional (RTXDI's infinite region)
    uint32_t localLightCount = 0;
    // GpuRayInstance[], 0 until the RT scene exists
    vk::DeviceAddress rayInstances = 0;
    // RTXDI constants, 0 until M3
    vk::DeviceAddress rtxdiConstants = 0;
    // GpuNrcConstants of this frame slot (M4 G3)
    vk::DeviceAddress cacheConstants = 0;
    uint32_t frameIndex = 0;
    // scale of the DLSS-RR color input; 1 ships (considerations C-H2-1), the settings lab tries others (m8 I16)
    float exposure = 1.0f;
    // GpuLight[lightCount] in world space, written this frame on the async compute queue (WorldLights)
    vk::DeviceAddress worldLights = 0;
    // last frame's worldLights, same rows; 0 on a history reset
    vk::DeviceAddress prevWorldLights = 0;
    // GpuRayDebugCounters of this frame slot; 0 unless kDebugRays
    vk::DeviceAddress rayDebug = 0;
    // GpuMaterialBins, then the per-bin tile lists (RenderTargets); one copy for both frame slots
    vk::DeviceAddress materialBins = 0;
    // uint[], parallel to rayInstances: the instance's first row in the triangle region, kNoneIndex unless its
    // material emits (RayTracingScene)
    vk::DeviceAddress rayInstanceLights = 0;
    // world row of triangle light 0: the point and spot rows come first
    uint32_t firstTriangleLight = 0;
    uint32_t triangleLightCount = 0;
    // GlassFlag (m8 I13); 0 without glass instances or with the glass pass off
    uint32_t glassFlags = 0;
    // layers the glass pass and the shadow transmittance cross at most
    uint32_t glassMaxLayers = 0;
    // GpuGlassCounters of this frame slot
    vk::DeviceAddress glassCounters = 0;
    // > 0: passes/combine.slang clamps the DLSS-RR input's luminance to this multiple of a reference (full_testing T11)
    float preDenoiserClamp = 0.0f;
    // > 0: passes/combine.slang blends the DLSS-RR input into its running average by this weight (settings lab, m8 I16;
    // still camera only: any motion restarts it)
    float inputHistoryWeight = 0.0f;
};
static_assert(sizeof(GpuFrameRoot) == 248);
static_assert(offsetof(GpuFrameRoot, preDenoiserClamp) == 240);
static_assert(offsetof(GpuFrameRoot, glassFlags) == 224);
static_assert(offsetof(GpuFrameRoot, glassCounters) == 232);
static_assert(offsetof(GpuFrameRoot, rayInstanceLights) == 208);
static_assert(offsetof(GpuFrameRoot, firstTriangleLight) == 216);
static_assert(offsetof(GpuFrameRoot, triangleLightCount) == 220);
static_assert(offsetof(GpuFrameRoot, materialBins) == 200);
static_assert(offsetof(GpuFrameRoot, camera) == 0);
static_assert(offsetof(GpuFrameRoot, instances) == 8);
static_assert(offsetof(GpuFrameRoot, models) == 16);
static_assert(offsetof(GpuFrameRoot, primitives) == 24);
static_assert(offsetof(GpuFrameRoot, materials) == 32);
static_assert(offsetof(GpuFrameRoot, pbrExtensions) == 40);
static_assert(offsetof(GpuFrameRoot, lights) == 48);
static_assert(offsetof(GpuFrameRoot, tlas) == 56);
static_assert(offsetof(GpuFrameRoot, nodeInstances) == 64);
static_assert(offsetof(GpuFrameRoot, drawRecords) == 72);
static_assert(offsetof(GpuFrameRoot, visibleMeshlets) == 80);
static_assert(offsetof(GpuFrameRoot, lightCount) == 88);
static_assert(offsetof(GpuFrameRoot, instanceCount) == 92);
static_assert(offsetof(GpuFrameRoot, primitiveCount) == 96);
static_assert(offsetof(GpuFrameRoot, nodeInstanceCount) == 100);
static_assert(offsetof(GpuFrameRoot, lateVisibleBase) == 104);
static_assert(offsetof(GpuFrameRoot, debugView) == 108);
static_assert(offsetof(GpuFrameRoot, cullCounters) == 112);
static_assert(offsetof(GpuFrameRoot, triangleCull) == 120);
static_assert(offsetof(GpuFrameRoot, subpixelSnap) == 124);
static_assert(offsetof(GpuFrameRoot, targets) == 128);
static_assert(offsetof(GpuFrameRoot, frameFlags) == 136);
static_assert(offsetof(GpuFrameRoot, localLightCount) == 140);
static_assert(offsetof(GpuFrameRoot, worldLights) == 176);
static_assert(offsetof(GpuFrameRoot, prevWorldLights) == 184);
static_assert(offsetof(GpuFrameRoot, rayDebug) == 192);
static_assert(offsetof(GpuFrameRoot, rayInstances) == 144);
static_assert(offsetof(GpuFrameRoot, rtxdiConstants) == 152);
static_assert(offsetof(GpuFrameRoot, cacheConstants) == 160);
static_assert(offsetof(GpuFrameRoot, frameIndex) == 168);
static_assert(offsetof(GpuFrameRoot, exposure) == 172);


struct HardwareCapabilities
{
    // Core/core-promoted properties
    vk::PhysicalDeviceProperties2 properties2;
    vk::PhysicalDeviceVulkan11Properties vulkan11;
    vk::PhysicalDeviceVulkan12Properties vulkan12;
    vk::PhysicalDeviceVulkan13Properties vulkan13;
    vk::PhysicalDeviceVulkan14Properties vulkan14;

    // Extension / advanced properties (as requested)
    vk::PhysicalDeviceBlendOperationAdvancedPropertiesEXT blendOperationAdvanced;
    vk::PhysicalDeviceDescriptorHeapPropertiesEXT descriptorHeap;
    vk::PhysicalDeviceDescriptorIndexingPropertiesEXT descriptorIndexing;
    vk::PhysicalDeviceMeshShaderPropertiesEXT meshShader;
    vk::PhysicalDeviceDeviceGeneratedCommandsPropertiesEXT deviceGeneratedCommands;
    vk::PhysicalDeviceMemoryDecompressionPropertiesEXT memoryDecompression;
    vk::PhysicalDeviceHostImageCopyPropertiesEXT hostImageCopy;
    std::vector<vk::ImageLayout> hostImageCopySrcLayouts;
    std::vector<vk::ImageLayout> hostImageCopyDstLayouts;
    vk::PhysicalDeviceTexelBufferAlignmentPropertiesEXT texelBufferAlignment;

    // KHR / other properties
    vk::PhysicalDeviceFragmentShadingRatePropertiesKHR fragmentShadingRate;
    vk::PhysicalDeviceAccelerationStructurePropertiesKHR accelerationStructure;
    vk::PhysicalDeviceDepthStencilResolveProperties depthStencilResolve;
    vk::PhysicalDeviceDriverProperties driverProperties;
    vk::PhysicalDeviceMaintenance3Properties maintenance3;
    vk::PhysicalDeviceMaintenance4Properties maintenance4;
    vk::PhysicalDeviceMaintenance5Properties maintenance5;
    vk::PhysicalDeviceMaintenance6Properties maintenance6;
    vk::PhysicalDeviceMaintenance7PropertiesKHR maintenance7;
    vk::PhysicalDeviceMaintenance9PropertiesKHR maintenance9;
    vk::PhysicalDeviceMaintenance10PropertiesKHR maintenance10;
    vk::PhysicalDevicePipelineBinaryPropertiesKHR pipelineBinary;
    vk::PhysicalDeviceRayTracingPipelinePropertiesKHR rayTracingPipeline;
    vk::PhysicalDeviceOpacityMicromapPropertiesEXT opacityMicromap;
    vk::PhysicalDeviceRayTracingInvocationReorderPropertiesEXT rayTracingInvocationReorder;
    vk::PhysicalDeviceClusterAccelerationStructurePropertiesNV clusterAccelerationStructure;
    vk::PhysicalDevicePartitionedAccelerationStructurePropertiesNV partitionedAccelerationStructure;

    // ReorderThread regroups threads; eNone devices (expected: RTX 20/30) run it as a no-op
    bool isSerReordering = false;
};
