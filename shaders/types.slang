module types;

// scalar layout: must match src/core/types.hpp + MeshPushData (slangc -fvk-use-scalar-layout)

public static const uint kNoneIndex = 0xffffffffu;
public static const uint kMaxMeshletVertices = 64;
public static const uint kMaxMeshletTriangles = 126;
public static const uint kGroupSize = 32;
// must match src/core/types.hpp kMeshletGroupMaxSize
public static const uint kMeshletGroupMaxSize = 32;

// must match src/core/types.hpp kCullGroupSize / kRasterBinCount
public static const uint kCullGroupSize = 64;
public static const uint kRasterBinCount = 2;
public static const uint kRasterBinAlphaTested = 1;

public struct GpuCameraData
{
    public float4x4 view;
    public float4x4 proj;
    public float4x4 viewProj;

    public float4x4 invView;
    public float4x4 invProj;
    public float4x4 invViewProj;

    public float4x4 prevViewProj;

    public float3 cameraPos;
    public float nearZ;

    public float2 renderTargetSize;
    public float2 invRenderTargetSize;

    public float2 jitterOffset;
    public float farZ;
    public float frameDeltaTime;

    public float4 cameraParams;
};

public struct GpuObjectUB
{
    public float4x4 modelMatrix;
    public float4x4 prevModelMatrix;

    public float4 boundingSphere;

    public uint modelIndex;
    public uint instanceFlags;
    public uint pad0;
    public uint pad1;
};

// slice addresses of one model; data inside a slice is model-relative
public struct GpuModel
{
    public uint64_t vertices;
    public uint64_t meshlets;
    public uint64_t meshletBounds; // GpuMeshletBounds[], parallel to meshlets
    public uint64_t meshletGroups; // GpuMeshletGroup[]
    public uint64_t meshletPrimitives; // uint per meshlet: its row of primitives
    public uint64_t meshletVertices;
    public uint64_t meshletTriangles;
    public uint64_t normals;
    public uint64_t tangents;
    public uint64_t uv1;
    public uint64_t indices;
    public uint64_t nodes;        // GpuNode[nodeCount]
    public uint64_t primitives;   // GpuPrimitive[], the model's slice of GpuFrameRoot.primitives
    public uint attributeMask;
    public uint nodeCount;
};

public static const uint kNodeFlagFlipWinding = 1u;

// GpuModel.attributeMask; must match GpuModelAttribute in src/core/types.hpp
public static const uint kModelAttributeNormals = 1u << 0;
public static const uint kModelAttributeTangents = 1u << 1;
public static const uint kModelAttributeUv1 = 1u << 2;

// GpuFrameRoot.debugView; must match src/render/debug_view.hpp
public static const uint kDebugViewLit = 0u;
public static const uint kDebugViewMeshlets = 1u;
public static const uint kDebugViewTriangles = 2u;
public static const uint kDebugViewPrimitives = 3u;
public static const uint kDebugViewNodeInstances = 4u;
public static const uint kDebugViewBins = 5u;
public static const uint kDebugViewCullPhase = 6u;
public static const uint kDebugViewHiZPyramid = 7u;
public static const uint kDebugViewDepth = 8u;
public static const uint kDebugViewShadingNormal = 9u;
public static const uint kDebugViewRoughness = 10u;
public static const uint kDebugViewGeometricNormal = 11u;
public static const uint kDebugViewDiffuseAlbedo = 12u;
public static const uint kDebugViewSpecularF0 = 13u;
public static const uint kDebugViewSpecularAlbedo = 14u;
public static const uint kDebugViewEmissive = 15u;
public static const uint kDebugViewMotionVectors = 16u;
public static const uint kDebugViewReprojection = 17u;
// primary rays through the TLAS (plan C3): the raster views they must match
public static const uint kDebugViewRayMeshlets = 18u;
public static const uint kDebugViewRayNodeInstances = 19u;
public static const uint kDebugViewRayShadingNormal = 20u;
public static const uint kDebugViewModeMask = 0xffu;
public static const uint kDebugViewFlagEdges = 1u << 8;

// one mesh placement inside its model, model-relative
public struct GpuNode
{
    public float4 localToModel[3]; // affine rows
    public float4 boundingSphere;  // model space
    public uint firstPrimitive;
    public uint primitiveCount;
    public uint flags;
    public uint pad0;
    public float3 bboxMin; // mesh space, tested under GpuNodeInstance.worldMatrix
    public uint pad1;
    public float3 bboxMax;
    public uint pad2;
};

// one entity x node, transforms precomputed per frame
public struct GpuNodeInstance
{
    public float4x4 worldMatrix;
    public float4x4 prevWorldMatrix;
    public uint entityIndex;
    public uint modelIndex;
    public uint nodeIndex;
    public uint flags;
};

// DrawRecordFlag
public static const uint kDrawRecordBinMask = 0x3u;
public static const uint kDrawRecordTwoSided = 1u << 2;

// one render-list entry (vk_lod_clusters ClusterInfo); must match src/core/types.hpp
public struct GpuVisibleMeshlet
{
    public uint nodeInstanceIndex;
    public uint meshletRow; // row of the model's GpuMeshletDesc slice
};

public enum AlphaMode : uint { Opaque = 0, Mask = 1, Blend = 2 }
// matches GpuLightType in src/core/types.hpp
public enum LightType : uint { Directional = 0, Point = 1, Spot = 2 }

// GpuMaterial.flags; must match GpuMaterialFlag in src/core/types.hpp
public static const uint kMaterialAlphaModeMask = 0x3u;
public static const uint kMaterialFlagDoubleSided = 1u << 2;
public static const uint kMaterialFlagUnlit = 1u << 3;
public static const uint kMaterialFlagUvTransformBaseColor = 1u << 4;
public static const uint kMaterialFlagUvTransformMetalRough = 1u << 5;
public static const uint kMaterialFlagUvTransformNormal = 1u << 6;
public static const uint kMaterialFlagUvTransformOcclusion = 1u << 7;
public static const uint kMaterialFlagUvTransformEmissive = 1u << 8;
public static const uint kMaterialUvSlotCount = 5;

// one meshlet cull work item: one GpuMeshletGroup of one primitive placed by one node instance
public struct GpuDrawRecord
{
    public uint nodeInstanceIndex;
    public uint primitiveIndex; // absolute row of GpuFrameRoot.primitives
    public uint meshletOffset;  // the group's first meshlet, relative to the primitive
    public uint flags;          // kDrawRecord*
    public uint groupOffset;    // relative to GpuPrimitive.firstGroup
};

// one node instance for the instance pass; its draw record templates are contiguous
public struct GpuInstanceCullItem
{
    public uint nodeInstanceIndex;
    public uint firstDrawRecord; // row of GpuCullView.drawRecordTemplates
    public uint drawRecordCount;
};

// CullFlag
public static const uint kCullFrustum = 1u << 0;
public static const uint kCullCone = 1u << 1;
public static const uint kCullStats = 1u << 2;
public static const uint kCullInstanceFrustum = 1u << 3;
public static const uint kCullOcclusion = 1u << 4;
public static const uint kCullLatePhase = 1u << 5;
public static const uint kCullSubPixel = 1u << 6;

// GpuFrameRoot.triangleCull, matches TriangleCullFlag in src/core/types.hpp
public static const uint kTriangleCullBackface = 1u << 0;
public static const uint kTriangleCullSmall = 1u << 1;
public static const uint kTriangleCullStats = 1u << 2;

public struct GpuCullView
{
    public float3 cameraPos;
    public uint flags; // kCull*
    public uint64_t visibleMeshlets;     // GpuVisibleMeshlet[], one region per raster bin
    public uint64_t counters;            // GpuCullCounters
    public uint64_t sequences;           // MeshDgcSequence[]
    public uint64_t instanceItems;       // GpuInstanceCullItem[instanceCount]
    public uint64_t drawRecordTemplates; // GpuDrawRecord[], copied to GpuFrameRoot.drawRecords
    public uint instanceCount;
    public uint maxMeshletsPerSequence;
    public uint binVisibleOffset[kRasterBinCount];
    public uint binSequenceOffset[kRasterBinCount];
    public float4x4 occlusionViewProj; // early: previous frame's, late: current
    public uint hizPyramid;            // resource heap, full mip chain
    public uint hizFarSampler;         // sampler heap: far-depth reduction
    public uint2 hizSize;              // mip 0
    public uint64_t instanceCandidates; // uint[]: instance items occluded early, re-tested late
    public uint64_t lateCounters;       // GpuCullCounters counting the candidates; early view only
    public uint drawRecordBase;         // first GpuFrameRoot.drawRecords row of this phase
    public uint rejectRecordBase;       // first row of the early phase's records, which rejectMasks is indexed by
    public uint64_t rejectMasks;        // uint per early draw record: meshlets occluded early, re-tested late
    public float4x4 cullViewProj;       // frustum and sub-pixel tests: current, or the frozen cull camera
    public float2 targetSize;           // render target in pixels
    public float subpixelSnap;          // pixels a vertex may move when snapped to the subpixel grid
    public uint instanceCandidateCapacity; // list capacities; appends past them are dropped and counted
    public uint drawRecordCapacity;        // rows of this phase's drawRecords region, also the reject masks
    public uint binVisibleCapacity[kRasterBinCount];
    public uint pad1;
};

public struct GpuCullCounters
{
    public uint visibleMeshlets[kRasterBinCount];
    public uint sequenceCount[kRasterBinCount];
    public uint meshletDispatch[3]; // VkDispatchIndirectCommand: x = draw records of visible instances
    public uint instanceDispatch[3]; // VkDispatchIndirectCommand: x = late candidates
    public uint meshletsRetested; // late, kCullStats only: reject-mask bits re-tested
    public uint instancesCulled; // frustum
    public uint instancesDrawn;
    public uint instancesOccluded;
    public uint testedMeshlets;
    public uint frustumCulled;
    public uint coneCulled;
    public uint occlusionCulled; // early: set in the reject masks, late: still occluded
    public uint trianglesTested; // mesh shader, kTriangleCullStats only
    public uint trianglesBackface;
    public uint trianglesSmall;
    public uint instancesSubPixel; // kCullStats only
    public uint meshletsSubPixel;
    public uint groupsTested; // kCullStats only
    public uint groupsCulled; // whole group by its box: frustum, sub-pixel or occluded
    public uint candidatesOverflowed; // always counted: appends dropped at capacity
    public uint drawRecordsOverflowed;
    public uint visibleOverflowed;
};

// values are relative to the owning model's slices
public struct GpuPrimitive
{
    public uint firstMeshlet;
    public uint meshletCount;
    public uint materialId; // absolute row of GpuFrameRoot.materials, kNoneIndex = default material (row 0)
    public uint firstVertex;
    public uint vertexCount;
    public uint firstIndex;
    public uint indexCount;
    public uint firstGroup;       // rows of model.meshletGroups
    public uint groupCount;
    public uint flags;            // kPrimitiveFlag*
};

// must match src/core/types.hpp GpuPrimitiveFlag
public static const uint kPrimitiveFlagDoubleSided = 1u << 0;

public struct GpuVertex
{
    public float3 pos;
    public float3 color;
    public float2 texCoord;
};

public struct GpuMeshletDesc
{
    public uint vertexOffset;
    public uint triangleOffset;
    public uint8_t vertexCount;
    public uint8_t triangleCount;
    public uint8_t pad0;
    public uint8_t pad1;
    // normal cone in object space, snorm8 (x / 127)
    public int8_t coneAxisX;
    public int8_t coneAxisY;
    public int8_t coneAxisZ;
    public int8_t coneCutoff;
    public float4 boundingSphere;
};

// a run of a primitive's meshlets culled as one box first
public struct GpuMeshletGroup
{
    public float3 bboxMin; // object space, union of the meshlet boxes
    public uint firstMeshlet; // relative to the primitive
    public float3 bboxMax;
    public uint meshletCount; // <= kMeshletGroupMaxSize
};

// object-space box of one meshlet, parallel to GpuMeshletDesc
public struct GpuMeshletBounds
{
    public float3 bboxMin;
    public float shortestEdge; // shortest and longest triangle edge, object space
    public float3 bboxMax;
    public float longestEdge;
};

// rows of M = T * R * S: uv' = M * (uv, 1), gradients take the 2x2 part
public struct GpuUvMatrix
{
    public float3 row0;
    public float3 row1;
};

public struct GpuMaterial
{
    public float4 baseColorFactor;
    public float3 emissiveFactor;
    public float metallicFactor;
    public float roughnessFactor;
    public float normalScale;
    public float occlusionStrength;
    public float alphaCutoff;
    public uint flags;

    public uint baseColorTex;
    public uint metalRoughTex;
    public uint normalTex;
    public uint occlusionTex;
    public uint emissiveTex;

    public uint baseColorSamp;
    public uint metalRoughSamp;
    public uint normalSamp;
    public uint occlusionSamp;
    public uint emissiveSamp;

    public uint8_t baseColorUv;
    public uint8_t metalRoughUv;
    public uint8_t normalUv;
    public uint8_t occlusionUv;
    public uint8_t emissiveUv;

    // C++ alignas(16) stride is 240 B
    public uint8_t pad0;
    public uint8_t pad1;
    public uint8_t pad2;
    public uint3 pad3;

    // valid only where the slot's kMaterialFlagUvTransform* bit is set
    public GpuUvMatrix uvMatrices[kMaterialUvSlotCount];
    public uint2 pad4;
};

// model space; loadWorldLight moves it to world space through its owning instance
public struct GpuLight
{
    public float3 position;
    public float range;
    public float3 direction;
    public float intensity;
    public float3 color;
    public LightType type;
    public float innerCone;
    public float outerCone;
    public uint instanceIndex;
    public float pad0;
};

public struct GpuUvTransform
{
    public float2 offset;
    public float2 scale;
    public float rotation;
};

public struct GpuTextureRef
{
    public uint tex;  // kNoneIndex = absent
    public uint samp;
    public uint uv;
    public float normalScale;
    public GpuUvTransform uvXform;
};

// 724 B, one row per GpuMaterial row (same index)
public struct GpuMaterialExtension
{
    public uint flags; // MaterialExtFlag::*

    public GpuUvTransform baseColorUv;
    public GpuUvTransform metalRoughUv;
    public GpuUvTransform normalUv;
    public GpuUvTransform occlusionUv;
    public GpuUvTransform emissiveUv;

    public float ior;
    public float emissiveStrength;
    public float dispersion;

    public float specularFactor;
    public float3 specularColorFactor;
    public GpuTextureRef specular;
    public GpuTextureRef specularColor;

    public float clearcoatFactor;
    public float clearcoatRoughnessFactor;
    public GpuTextureRef clearcoat;
    public GpuTextureRef clearcoatRoughness;
    public GpuTextureRef clearcoatNormal;

    public float3 sheenColorFactor;
    public float sheenRoughnessFactor;
    public GpuTextureRef sheenColor;
    public GpuTextureRef sheenRoughness;

    public float transmissionFactor;
    public GpuTextureRef transmission;

    public float thicknessFactor;
    public float attenuationDistance; // 0 = infinite
    public float3 attenuationColor;
    public GpuTextureRef thickness;

    public float iridescenceFactor;
    public float iridescenceIor;
    public float iridescenceThicknessMin;
    public float iridescenceThicknessMax;
    public GpuTextureRef iridescence;
    public GpuTextureRef iridescenceThickness;

    public float anisotropyStrength;
    public float anisotropyRotation;
    public GpuTextureRef anisotropy;

    public float diffuseTransmissionFactor;
    public float3 diffuseTransmissionColorFactor;
    public GpuTextureRef diffuseTransmission;
    public GpuTextureRef diffuseTransmissionColor;
};

// 16 B, matches src/render/push_data.hpp
public struct MeshPushData
{
    public uint64_t frameRoot;
    public uint firstVisible; // row of GpuFrameRoot.visibleMeshlets for group 0
    public uint meshletCount; // groups in this batch
};

// one DGC sequence: PUSH_DATA + DRAW_MESH_TASKS, 32 B stride
public struct MeshDgcSequence
{
    public MeshPushData push;
    public uint3 groupCount;
    public uint pad0;
};

// 16 B, matches src/render/push_data.hpp
public struct CullPushData
{
    public uint64_t frameRoot;
    public uint64_t cullView; // GpuCullView
};

// 24 B, matches src/render/push_data.hpp
public struct HiZViewPushData
{
    public uint pyramid; // resource heap, full mip chain
    public uint mip;
    public float nearZ;
    public float farZ;
    public uint2 screenSize;
};

// 48 B, matches src/render/push_data.hpp
public struct HiZPushData
{
    public uint srcImage;     // resource heap: depth for mip 0, the pyramid's full-chain view otherwise
    public uint dstImages[3]; // resource heap: storage views of the written mips, first levelCount valid
    public uint srcMip;       // pyramid mip the first written level halves; unused from depth
    public uint isFromDepth;
    public uint levelCount;
    public uint pad0;
    public uint2 srcSize;
    public uint2 dstSize;     // first written level
};

// 8 B, matches src/render/push_data.hpp: fullscreen passes reach everything through the root
public struct FramePushData
{
    public uint64_t frameRoot;
};

// 16 B, matches src/render/push_data.hpp
public struct MotionCheckPushData
{
    public uint64_t frameRoot;
    public uint64_t stats; // GpuMotionCheckStats
};

// 16 B, matches src/render/push_data.hpp
public struct RayCheckPushData
{
    public uint64_t frameRoot;
    public uint64_t stats; // GpuRayCheckStats
};

// 68 B, matches src/core/types.hpp: rt_check pass, ENGINE_RT_CHECK only. max* fields are float bits
public struct GpuRayCheckStats
{
    public uint coveredPixels;
    public uint interiorPixels; // covered, and the 4 neighbours hold the same payload
    public uint misses;         // interior, the primary ray hits nothing
    public uint idMismatches;   // interior, another (node instance, meshlet, triangle)
    public uint backFaceHits;   // the ray hits a back face of a one-sided primitive, which raster culls
    public uint edgeMismatches; // covered but not interior: another id along triangle edges
    public uint emptyHits;      // empty pixel with empty neighbours, the ray hits something
    public uint selfHits;       // a spawned ray from a matched hit hits its own triangle
    public uint mipOutliers;    // matched, ray-cone and raster uv footprints more than half a mip apart
    public uint depthTies;      // interior, another triangle within intersection noise of the raster one: coplanar
    // the decode at the resolve's barycentrics and gradients: any difference is the decode chain's
    public uint maxNormalErrorBits;         // degrees, Surface.normal
    public uint maxGeoNormalErrorBits;      // degrees
    public uint maxPositionErrorBits;       // world distance between the two intersections / hit distance
    public uint maxMipErrorBits;            // |log2| of the uv footprint ratio, uv set 0, ray cone vs raster
    public uint maxMaterialNormalErrorBits; // degrees
    // the same at the ray's own barycentrics: intersection noise through steep normal maps, reported only
    public uint maxHitNormalErrorBits;
    public uint maxHitMaterialNormalErrorBits;
};

// 40 B, matches src/core/types.hpp: motion_check pass, ENGINE_MOTION_TEST only
public struct GpuMotionCheckStats
{
    public uint pixels;
    public uint emptyPixels;
    public uint movingPixels;
    public uint cameraMismatches;
    public uint reprojectedErrors;
    public uint stillErrors;
    public uint offscreenPixels;
    public uint frameFlags;
    public uint maxMotionBits;      // float bits
    public uint maxCameraErrorBits; // float bits
};

// 32 B, matches src/render/push_data.hpp: rt_instances pass
public struct RtInstancesPushData
{
    public uint64_t frameRoot;
    public uint64_t blasAddresses; // uint64 per ray instance, parallel to GpuFrameRoot.rayInstances
    public uint64_t tlasInstances; // VkAccelerationStructureInstanceKHR[count]
    public uint count;
    public uint pad0;
};

// 16 B, matches src/render/push_data.hpp
public struct PresentPushData
{
    public uint64_t frameRoot;
    public uint2 displaySize; // swapchain extent
};

// vis-buffer raster: position only, the payload is per primitive
public struct MeshVertexOut
{
    public float4 pos : SV_Position;
};

// must match src/core/types.hpp kVisTriangleBits
public static const uint kVisTriangleBits = 7;

// GpuGuideSet rows; must match GuideImage in src/core/types.hpp
public static const uint kGuideNormalRoughness = 0;
public static const uint kGuideGeoNormal = 1;
public static const uint kGuideDiffuseAlbedo = 2;
public static const uint kGuideSpecularF0 = 3;
public static const uint kGuideSpecAlbedo = 4;
public static const uint kGuideEmissive = 5;
public static const uint kGuideMotion = 6;
public static const uint kGuideImageCount = 7;

public struct GpuGuideSet
{
    public uint storage[kGuideImageCount]; // written by the resolve
    public uint sampled[kGuideImageCount];
};

// render-resolution targets of one frame; matches GpuTargets in src/core/types.hpp
public struct GpuTargets
{
    public uint visBuffer; // sampled R32_UINT
    public uint depth;     // sampled, reverse Z
    public uint displayStorage;
    public uint displaySampled;
    public uint linearSampler; // sampler heap
    public uint pad0;
    public uint2 renderSize;
    public GpuGuideSet current; // this frame's, by frame parity
    public GpuGuideSet previous;
};

// [InstanceIndex()] of a TLAS instance; written by the CPU per scene layout (RayTracingScene)
public struct GpuRayInstance
{
    public uint nodeInstanceIndex; // row in GpuFrameRoot.nodeInstances
    public uint primitiveRow;      // row in GpuFrameRoot.primitives
};

// GpuFrameRoot.frameFlags, matches FrameFlag in src/core/types.hpp
public static const uint kFrameFlagHistoryReset = 1u << 0;

// per-frame root, matches GpuFrameRoot in src/core/types.hpp
public struct GpuFrameRoot
{
    public uint64_t camera;
    public uint64_t instances;     // GpuObjectUB[entity]
    public uint64_t models;        // GpuModel[model]
    public uint64_t primitives;    // GpuPrimitive[], absolute rows
    public uint64_t materials;     // whole GpuMaterial[]; only row 0 (default) is read here
    public uint64_t pbrExtensions; // whole GpuMaterialExtension[], parallel to materials
    public uint64_t lights;        // GpuLight[lightCount]
    public uint64_t tlas;
    public uint64_t nodeInstances; // GpuNodeInstance[nodeInstanceCount]
    public uint64_t drawRecords;     // GpuDrawRecord[], written by the instance cull pass
    public uint64_t visibleMeshlets; // GpuVisibleMeshlet[]
    public uint lightCount;
    public uint instanceCount;
    public uint primitiveCount;
    public uint nodeInstanceCount;
    public uint lateVisibleBase; // first visibleMeshlets row of the late phase
    public uint debugView; // kDebugView* mode in the low byte, kDebugViewFlag* above
    public uint64_t cullCounters; // GpuCullCounters[kCullPhaseCount]; the mesh shader adds its triangle counts
    public uint triangleCull;     // kTriangleCull*
    public float subpixelSnap;    // pixels a vertex may move when snapped to the subpixel grid
    public uint64_t targets;      // GpuTargets
    public uint frameFlags;       // kFrameFlag*
    public uint pad0;
    public uint64_t rayInstances;   // GpuRayInstance[], 0 until the RT scene exists
    public uint64_t rtxdiConstants; // 0 until M3
    public uint64_t cacheConstants; // SHaRC or NRC constants, 0 until M4
    public uint frameIndex;
    public float exposure;          // pre-exposure, 1 until auto exposure
};
