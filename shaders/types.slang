#ifndef ENGINE_TYPES_SLANG
#define ENGINE_TYPES_SLANG

// scalar layout: must match src/core/types.hpp + MeshPushData (slangc -fvk-use-scalar-layout)

static const uint kNoneIndex = 0xffffffffu;
static const uint kMaxMeshletVertices = 64;
static const uint kMaxMeshletTriangles = 126;
static const uint kGroupSize = 32;
// must match src/core/types.hpp kMeshletGroupMaxSize
static const uint kMeshletGroupMaxSize = 32;

// must match src/core/types.hpp kCullGroupSize / kMeshStateGroupCount
static const uint kCullGroupSize = 64;
static const uint kMeshStateGroupCount = 2;
static const uint kMeshStateGroupDoubleSided = 1;

struct GpuCameraData
{
    float4x4 view;
    float4x4 proj;
    float4x4 viewProj;

    float4x4 invView;
    float4x4 invProj;
    float4x4 invViewProj;

    float4x4 prevViewProj;

    float3 cameraPos;
    float nearZ;

    float2 renderTargetSize;
    float2 invRenderTargetSize;

    float2 jitterOffset;
    float farZ;
    float frameDeltaTime;

    float4 cameraParams;
};

struct GpuObjectUB
{
    float4x4 modelMatrix;
    float4x4 prevModelMatrix;

    float4 boundingSphere;

    uint modelIndex;
    uint instanceFlags;
    uint pad0;
    uint pad1;
};

// slice addresses of one model; data inside a slice is model-relative
struct GpuModel
{
    uint64_t vertices;
    uint64_t meshlets;
    uint64_t meshletBounds; // GpuMeshletBounds[], parallel to meshlets
    uint64_t meshletGroups; // GpuMeshletGroup[]
    uint64_t meshletPrimitives; // uint per meshlet: its row of primitives
    uint64_t meshletVertices;
    uint64_t meshletTriangles;
    uint64_t normals;
    uint64_t tangents;
    uint64_t uv1;
    uint64_t joints;
    uint64_t weights;
    uint64_t indices;
    uint64_t morphPos;     // float3 deltas
    uint64_t morphNrm;     // float3 deltas
    uint64_t morphTan;     // float4 deltas
    uint64_t morphTargets; // GpuMorphTarget[]
    uint64_t morphWeights; // float[], glTF default weights
    uint64_t nodes;        // GpuNode[nodeCount]
    uint64_t primitives;   // GpuPrimitive[], the model's slice of GpuFrameRoot.primitives
    uint attributeMask;
    uint nodeCount;
};

static const uint kNodeFlagFlipWinding = 1u;

// GpuFrameRoot.debugView; must match src/render/debug_view.hpp
static const uint kDebugViewLit = 0u;
static const uint kDebugViewMeshlets = 1u;
static const uint kDebugViewTriangles = 2u;
static const uint kDebugViewPrimitives = 3u;
static const uint kDebugViewNodeInstances = 4u;
static const uint kDebugViewStateGroups = 5u;
static const uint kDebugViewCullPhase = 6u;
static const uint kDebugViewModeMask = 0xffu;
static const uint kDebugViewFlagEdges = 1u << 8;

// one mesh placement inside its model, model-relative
struct GpuNode
{
    float4 localToModel[3]; // affine rows
    float4 boundingSphere;  // model space
    uint firstPrimitive;
    uint primitiveCount;
    uint flags;
    uint pad0;
    float3 bboxMin; // mesh space, tested under GpuNodeInstance.worldMatrix
    uint pad1;
    float3 bboxMax;
    uint pad2;
};

// one entity x node, transforms precomputed per frame
struct GpuNodeInstance
{
    float4x4 worldMatrix;
    float4x4 prevWorldMatrix;
    uint entityIndex;
    uint modelIndex;
    uint nodeIndex;
    uint flags;
};

// DrawRecordFlag
static const uint kDrawRecordStateGroupMask = 0x3u;
static const uint kDrawRecordSkipCulling = 1u << 2;

// one render-list entry (vk_lod_clusters ClusterInfo); must match src/core/types.hpp
struct GpuVisibleMeshlet
{
    uint nodeInstanceIndex;
    uint meshletRow; // row of the model's GpuMeshletDesc slice
};

// GpuMaterial.flags; must match GpuMaterialFlag in src/core/types.hpp
static const uint kMaterialFlagDoubleSided = 1u << 2;

// one meshlet cull work item: one GpuMeshletGroup of one primitive placed by one node instance
struct GpuDrawRecord
{
    uint nodeInstanceIndex;
    uint primitiveIndex; // absolute row of GpuFrameRoot.primitives
    uint meshletOffset;  // the group's first meshlet, relative to the primitive
    uint flags;          // kDrawRecord*
    uint groupOffset;    // relative to GpuPrimitive.firstGroup
};

// one node instance for the instance pass; its draw record templates are contiguous
struct GpuInstanceCullItem
{
    uint nodeInstanceIndex;
    uint firstDrawRecord; // row of GpuCullView.drawRecordTemplates
    uint drawRecordCount;
    uint flags;           // kDrawRecordSkipCulling when any primitive is morphed
};

// CullFlag
static const uint kCullFrustum = 1u << 0;
static const uint kCullCone = 1u << 1;
static const uint kCullStats = 1u << 2;
static const uint kCullInstanceFrustum = 1u << 3;
static const uint kCullOcclusion = 1u << 4;
static const uint kCullLatePhase = 1u << 5;
static const uint kCullSubPixel = 1u << 6;

// GpuFrameRoot.triangleCull, matches TriangleCullFlag in src/core/types.hpp
static const uint kTriangleCullBackface = 1u << 0;
static const uint kTriangleCullSmall = 1u << 1;
static const uint kTriangleCullStats = 1u << 2;

struct GpuCullView
{
    float3 cameraPos;
    uint flags; // kCull*
    uint64_t visibleMeshlets;     // GpuVisibleMeshlet[], one region per state group
    uint64_t counters;            // GpuCullCounters
    uint64_t sequences;           // MeshDgcSequence[]
    uint64_t instanceItems;       // GpuInstanceCullItem[instanceCount]
    uint64_t drawRecordTemplates; // GpuDrawRecord[], copied to GpuFrameRoot.drawRecords
    uint instanceCount;
    uint maxMeshletsPerSequence;
    uint groupVisibleOffset[kMeshStateGroupCount];
    uint groupSequenceOffset[kMeshStateGroupCount];
    float4x4 occlusionViewProj; // early: previous frame's, late: current
    uint hizPyramid;            // resource heap, full mip chain
    uint hizMaxSampler;         // sampler heap
    uint2 hizSize;              // mip 0
    uint64_t instanceCandidates; // uint[]: instance items occluded early, re-tested late
    uint64_t lateCounters;       // GpuCullCounters counting the candidates; early view only
    uint drawRecordBase;         // first GpuFrameRoot.drawRecords row of this phase
    uint rejectRecordBase;       // first row of the early phase's records, which rejectMasks is indexed by
    uint64_t rejectMasks;        // uint per early draw record: meshlets occluded early, re-tested late
    float4x4 cullViewProj;       // frustum and sub-pixel tests: current, or the frozen cull camera
    float2 targetSize;           // render target in pixels
    float subpixelSnap;          // pixels a vertex may move when snapped to the subpixel grid
    uint pad1;
};

struct GpuCullCounters
{
    uint visibleMeshlets[kMeshStateGroupCount];
    uint sequenceCount[kMeshStateGroupCount];
    uint meshletDispatch[3]; // VkDispatchIndirectCommand: x = draw records of visible instances
    uint instanceDispatch[3]; // VkDispatchIndirectCommand: x = late candidates
    uint meshletsRetested; // late, kCullStats only: reject-mask bits re-tested
    uint instancesCulled; // frustum
    uint instancesDrawn;
    uint instancesOccluded;
    uint testedMeshlets;
    uint frustumCulled;
    uint coneCulled;
    uint unculledMeshlets;
    uint occlusionCulled; // early: set in the reject masks, late: still occluded
    uint trianglesTested; // mesh shader, kTriangleCullStats only
    uint trianglesBackface;
    uint trianglesSmall;
    uint instancesSubPixel; // kCullStats only
    uint meshletsSubPixel;
    uint groupsTested; // kCullStats only
    uint groupsCulled; // whole group by its box: frustum, sub-pixel or occluded
};

// delta offsets relative to the model's morph slices, kNoneIndex = absent
struct GpuMorphTarget
{
    uint posOffset;
    uint nrmOffset;
    uint tanOffset;
};

// values are relative to the owning model's slices
struct GpuPrimitive
{
    uint firstMeshlet;
    uint meshletCount;
    uint materialId; // absolute row of GpuFrameRoot.materials, kNoneIndex = default material (row 0)
    uint firstVertex;
    uint vertexCount;
    uint firstIndex;
    uint indexCount;
    uint morphFirst;       // rows of model.morphTargets
    uint morphCount;
    uint morphWeightFirst; // row of model.morphWeights, kNoneIndex = no default weights
    uint firstGroup;       // rows of model.meshletGroups
    uint groupCount;
};

struct GpuVertex
{
    float3 pos;
    float3 color;
    float2 texCoord;
};

struct GpuMeshletDesc
{
    uint vertexOffset;
    uint triangleOffset;
    uint8_t vertexCount;
    uint8_t triangleCount;
    uint8_t pad0;
    uint8_t pad1;
    // normal cone in object space, snorm8 (x / 127)
    int8_t coneAxisX;
    int8_t coneAxisY;
    int8_t coneAxisZ;
    int8_t coneCutoff;
    float4 boundingSphere;
};

// a run of a primitive's meshlets culled as one box first
struct GpuMeshletGroup
{
    float3 bboxMin; // object space, union of the meshlet boxes
    uint firstMeshlet; // relative to the primitive
    float3 bboxMax;
    uint meshletCount; // <= kMeshletGroupMaxSize
};

// object-space box of one meshlet, parallel to GpuMeshletDesc
struct GpuMeshletBounds
{
    float3 bboxMin;
    float shortestEdge; // shortest and longest triangle edge, object space
    float3 bboxMax;
    float longestEdge;
};

struct GpuMaterial
{
    float4 baseColorFactor;
    float3 emissiveFactor;
    float metallicFactor;
    float roughnessFactor;
    float normalScale;
    float occlusionStrength;
    float alphaCutoff;
    uint flags;

    uint baseColorTex;
    uint metalRoughTex;
    uint normalTex;
    uint occlusionTex;
    uint emissiveTex;

    uint baseColorSamp;
    uint metalRoughSamp;
    uint normalSamp;
    uint occlusionSamp;
    uint emissiveSamp;

    uint8_t baseColorUv;
    uint8_t metalRoughUv;
    uint8_t normalUv;
    uint8_t occlusionUv;
    uint8_t emissiveUv;

    // C++ alignas(16) stride is 112 B
    uint8_t pad0;
    uint8_t pad1;
    uint8_t pad2;
    uint3 pad3;
};

// model space; loadWorldLight moves it to world space through its owning instance
struct GpuLight
{
    float3 position;
    float range;
    float3 direction;
    float intensity;
    float3 color;
    uint type;
    float innerCone;
    float outerCone;
    uint instanceIndex;
    float pad0;
};

struct GpuUvTransform
{
    float2 offset;
    float2 scale;
    float rotation;
};

struct GpuTextureRef
{
    uint tex;  // kNoneIndex = absent
    uint samp;
    uint uv;
    float normalScale;
    GpuUvTransform uvXform;
};

// 724 B, one row per GpuMaterial row (same index)
struct GpuMaterialExtension
{
    uint flags; // MaterialExtFlag::*

    GpuUvTransform baseColorUv;
    GpuUvTransform metalRoughUv;
    GpuUvTransform normalUv;
    GpuUvTransform occlusionUv;
    GpuUvTransform emissiveUv;

    float ior;
    float emissiveStrength;
    float dispersion;

    float specularFactor;
    float3 specularColorFactor;
    GpuTextureRef specular;
    GpuTextureRef specularColor;

    float clearcoatFactor;
    float clearcoatRoughnessFactor;
    GpuTextureRef clearcoat;
    GpuTextureRef clearcoatRoughness;
    GpuTextureRef clearcoatNormal;

    float3 sheenColorFactor;
    float sheenRoughnessFactor;
    GpuTextureRef sheenColor;
    GpuTextureRef sheenRoughness;

    float transmissionFactor;
    GpuTextureRef transmission;

    float thicknessFactor;
    float attenuationDistance; // 0 = infinite
    float3 attenuationColor;
    GpuTextureRef thickness;

    float iridescenceFactor;
    float iridescenceIor;
    float iridescenceThicknessMin;
    float iridescenceThicknessMax;
    GpuTextureRef iridescence;
    GpuTextureRef iridescenceThickness;

    float anisotropyStrength;
    float anisotropyRotation;
    GpuTextureRef anisotropy;

    float diffuseTransmissionFactor;
    float3 diffuseTransmissionColorFactor;
    GpuTextureRef diffuseTransmission;
    GpuTextureRef diffuseTransmissionColor;
};

// 16 B, matches src/render/push_data.hpp
struct MeshPushData
{
    uint64_t frameRoot;
    uint firstVisible; // row of GpuFrameRoot.visibleMeshlets for group 0
    uint meshletCount; // groups in this batch
};

// one DGC sequence: PUSH_DATA + DRAW_MESH_TASKS, 32 B stride
struct MeshDgcSequence
{
    MeshPushData push;
    uint3 groupCount;
    uint pad0;
};

// 16 B, matches src/render/push_data.hpp
struct CullPushData
{
    uint64_t frameRoot;
    uint64_t cullView; // GpuCullView
};

// 24 B, matches src/render/push_data.hpp
struct HiZViewPushData
{
    uint pyramid; // resource heap, full mip chain
    uint mip;
    float nearZ;
    float farZ;
    uint2 screenSize;
};

// 32 B, matches src/render/push_data.hpp
struct HiZPushData
{
    uint srcImage; // resource heap: depth for mip 0, the pyramid's full-chain view otherwise
    uint dstImage; // resource heap: storage view of the written mip
    uint maxSampler; // sampler heap: max reduction, clamp to edge
    uint srcMip;
    uint2 srcSize;
    uint2 dstSize;
};

struct MeshVertexOut
{
    float4 pos          : SV_Position;
    float3 fragColor    : COLOR;
    float2 fragTexCoord : TEXCOORD;
    // same for every vertex of a meshlet
    nointerpolation uint materialRow : MATERIAL_ROW;
};

// per-frame root, matches GpuFrameRoot in src/core/types.hpp
struct GpuFrameRoot
{
    uint64_t camera;
    uint64_t instances;     // GpuObjectUB[entity]
    uint64_t models;        // GpuModel[model]
    uint64_t primitives;    // GpuPrimitive[], absolute rows
    uint64_t materials;     // whole GpuMaterial[]; only row 0 (default) is read here
    uint64_t pbrExtensions; // whole GpuMaterialExtension[], parallel to materials
    uint64_t lights;        // GpuLight[lightCount]
    uint64_t tlas;
    uint64_t nodeInstances; // GpuNodeInstance[nodeInstanceCount]
    uint64_t drawRecords;     // GpuDrawRecord[], written by the instance cull pass
    uint64_t visibleMeshlets; // GpuVisibleMeshlet[]
    uint lightCount;
    uint instanceCount;
    uint primitiveCount;
    uint nodeInstanceCount;
    uint lateVisibleBase; // first visibleMeshlets row of the late phase
    uint debugView; // kDebugView* mode in the low byte, kDebugViewFlag* above
    uint64_t cullCounters; // GpuCullCounters[kCullPhaseCount]; the mesh shader adds its triangle counts
    uint triangleCull;     // kTriangleCull*
    float subpixelSnap;    // pixels a vertex may move when snapped to the subpixel grid
};

#endif
