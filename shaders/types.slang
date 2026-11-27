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

// MaterialBin in src/core/types.hpp: resolve shader per pixel, also the resolve execution-set index
public static const uint kMaterialBinBackground = 0;
public static const uint kMaterialBinStandard = 1;
public static const uint kMaterialBinUnlit = 2;
public static const uint kMaterialBinCount = 3;
// classify / resolve tile edge in pixels
public static const uint kMaterialTileSize = 8;

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

    public float3 prevCameraPos; // equal to cameraPos on a cut
    public float lodBias;        // log2(render / display) under DLSS (engine spec §12.2)

    // world-space ray basis at unit view depth for jittered pixel centers (m8 O4)
    public float3 rayOrigin;
    public float3 rayDx;
    public float3 rayDy;
    public float3 prevRayOrigin; // equal to rayOrigin on a cut
    public float3 prevRayDx;
    public float3 prevRayDy;
    public float rayPad0;
    public float rayPad1;
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
public static const uint kDebugViewAmbientOcclusion = 21u;
public static const uint kDebugViewUnshadowedLights = 22u;
public static const uint kDebugViewShadowedLights = 23u;
public static const uint kDebugViewDirectLighting = 24u;
public static const uint kDebugViewDlssInput = 25u;
public static const uint kDebugViewRtxdiReservoirs = 26u;
public static const uint kDebugViewIndirectSamples = 27u;
public static const uint kDebugViewNrcCache = 28u;
public static const uint kDebugViewIndirectRadiance = 29u;
public static const uint kDebugViewMaterialBin = 30u;
// the glass pass's throughput, or with kDebugViewFlagGlassReflection its reflection term (m8 I13)
public static const uint kDebugViewGlass = 31u;
// DLSS-RR's specular hit distance guide (m8 I16 phase 3a): gray d / (d + 1), blue for a sky miss, black without one
public static const uint kDebugViewSpecularHitDistance = 32u;
// SHaRC spatial hash radiance cache debug visualization (M8.4)
public static const uint kDebugViewSharcCache = 33u;
// R16F finite max: sky-miss sentinel and clamp ceiling for pInSpecularHitDistance (reference/vk_denoise_dlssrr)
public static const float kMaxSpecularHitDistance = 65504.0;
public static const uint kDebugViewModeMask = 0xffu;
public static const uint kDebugViewFlagEdges = 1u << 8;
public static const uint kDebugViewFlagGlassReflection = 1u << 9;

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
public enum LightType : uint { Directional = 0, Point = 1, Spot = 2, Triangle = 3 }

// GpuMaterial.flags; must match GpuMaterialFlag in src/core/types.hpp
public static const uint kMaterialAlphaModeMask = 0x3u;
public static const uint kMaterialFlagDoubleSided = 1u << 2;
public static const uint kMaterialFlagUnlit = 1u << 3;
public static const uint kMaterialFlagUvTransformBaseColor = 1u << 4;
public static const uint kMaterialFlagUvTransformMetalRough = 1u << 5;
public static const uint kMaterialFlagUvTransformNormal = 1u << 6;
public static const uint kMaterialFlagUvTransformOcclusion = 1u << 7;
public static const uint kMaterialFlagUvTransformEmissive = 1u << 8;
public static const uint kMaterialFlagSpecularGlossiness = 1u << 9;
// smooth thin-walled glass (m8 I13): out of the vis buffer, on kRayMaskGlass
public static const uint kMaterialFlagTransmissive = 1u << 10;
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
    public float subpixelSnap;          // pixels the raster may move a corner: grid snap plus this frame's jitter
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
    // read only under kMaterialFlagSpecularGlossiness
    public float3 specularFactor;

    // valid only where the slot's kMaterialFlagUvTransform* bit is set
    public GpuUvMatrix uvMatrices[kMaterialUvSlotCount];
    // read only under kMaterialFlagSpecularGlossiness
    public float glossinessFactor;
    public uint pad4;
};

// GpuFrameRoot.lights: model space, placed by passes/light_world.slang into GpuFrameRoot.worldLights each frame
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

// a world row of type Triangle, the GpuLight row reinterpreted; matches src/core/types.hpp
public struct GpuTriangleLight
{
    public float3 v0;
    public float3 edge1;
    public float3 edge2;
    public uint flags; // kTriangleLightFlag*
    public uint pad0;
    public LightType type;
    public float3 radiance;
    public uint pad1;
};

// GpuTriangleLight.flags; must match GpuTriangleLightFlag in src/core/types.hpp
public static const uint kTriangleLightFlagDoubleSided = 1u << 0;
public static const uint kTriangleLightFlagMirrored = 1u << 1;

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
public struct ResolvePushData
{
    public uint64_t frameRoot;
    public uint64_t tileList; // uint[] of the bin: tile x in the low 16 bits, y in the high
};

// one DGC sequence: EXECUTION_SET + PUSH_DATA + DISPATCH, 40 B stride
public struct ResolveDgcSequence
{
    public uint executionSetIndex; // kMaterialBin*
    public uint pad0;
    public ResolvePushData push;
    public uint3 groupCount;
    public uint pad1;
};

// head of the material-bin buffer, followed by kMaterialBinCount tile lists of one entry per render tile each
public struct GpuMaterialBins
{
    public uint tileCount[kMaterialBinCount];
    public uint sequenceCount; // DGC sequenceCountAddress
    public ResolveDgcSequence sequences[kMaterialBinCount];
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

// 64 B, matches src/render/push_data.hpp
public struct LightWorldPushData
{
    public uint64_t frameRoot;
    public float3 sunDirection;   // world space, the direction the light travels
    public float sunIlluminance;  // lux
    public float3 sunColor;
    public uint assetLightCount;  // asset rows, with the triangle region after their point and spot rows; then the sun
    public uint64_t triangleLightRefs; // GpuTriangleLightRef[triangleLightRefCount]
    public uint triangleLightRefCount;
    public uint lightPdf;           // resource heap: storage view of the local light PDF's mip 0
    public uint lightPdfTexelCount; // mip 0 texels; every one is written each frame
    public uint pad0;
};

// 16 B, matches src/render/push_data.hpp
public struct LightPdfMipsPushData
{
    public uint srcImage; // resource heap: storage view of the read mip
    public uint dstImage; // resource heap: storage view of the written mip
    public uint2 dstSize;
};

// matches src/core/types.hpp: what the trace helpers report under kDebugRays (raytracing/trace.slang)
public struct GpuRayDebugCounters
{
    public uint candidates; // queries still running after their one Proceed()
};

// 56 B, matches src/render/push_data.hpp: rt_instances pass
public struct RtInstancesPushData
{
    public uint64_t frameRoot;
    public uint64_t buildRows;  // GpuRayInstanceBuild[count], parallel to GpuFrameRoot.rayInstances
    public uint64_t writeInfos; // VkPartitionedAccelerationStructureWriteInstanceDataNV[count]
    public uint64_t operation;  // VkBuildPartitionedAccelerationStructureIndirectCommandNV, argCount appended
    public uint64_t ptlasState; // one state row per instance: the transform the PTLAS holds
    public uint count;
    public uint flags;          // kRtInstancesFlag*
};

// RtInstancesPushData.flags, matches RtInstancesFlag in src/render/push_data.hpp
public static const uint kRtInstancesFlagFullBuild = 1u << 0;

// 16 B, matches src/render/push_data.hpp
// ExposurePushData.flags, matches src/render/push_data.hpp ExposureFlag
public static const uint kExposureFlagAuto = 1u << 0;
public static const uint kExposureFlagSnap = 1u << 1;

// 56 B, matches src/render/push_data.hpp
public struct ExposurePushData
{
    public uint64_t frameRoot;
    public uint64_t histogram; // uint2[kExposureHistogramBins]
    public uint64_t readback;  // GpuExposureReadback
    public float deltaSeconds;
    public float compensationEv;
    public float manualEv;
    public float minEv;
    public float maxEv;
    public float adaptationSpeedUp;
    public float adaptationSpeedDown;
    public uint flags; // kExposureFlag*
    public uint input; // resource heap, sampled: the metered image at the display extent
    public uint pad0;
};

// 16 B, matches src/render/push_data.hpp
public struct PostPushData
{
    public uint64_t frameRoot;
    public uint input; // resource heap, sampled: the tonemapped image at the display extent
    public uint pad0;
};

// 24 B, matches src/render/push_data.hpp
public struct AccumulatePushData
{
    public uint input;       // resource heap: noisyColor, sampled
    public uint accumulated; // resource heap: RGBA32F running mean, storage
    public uint2 size;
    public float weight;     // 1 / frames averaged; 1 copies the input
    public uint pad0;
};

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

// alpha-tested bin (spec/m6/d3_alpha_raster_bin.md): the base-color uv with its KHR_texture_transform applied; the
// rest is uniform per meshlet, loaded once by its workgroup so the fragment reads no buffer
public struct MeshAlphaVertexOut
{
    public float4 pos : SV_Position;
    public float2 uv : TEXCOORD0;
    // base-color texture and sampler heap indices
    public nointerpolation uint2 baseColor : BASE_COLOR;
    // texture alpha below cutoff / factor.a is cut, as the OMM bake (alphaTestCutoff); lodBias as the resolve's
    public nointerpolation float2 alphaTest : ALPHA_TEST;
};

// must match src/core/types.hpp kVisTriangleBits
public static const uint kVisTriangleBits = 7;

// GpuGuideSet rows; must match GuideImage in src/core/types.hpp
public static const uint kGuideNormalRoughness = 0;
public static const uint kGuideDiffuseAlbedo = 1;
public static const uint kGuideSpecAlbedo = 2;
public static const uint kGuideEmissive = 3;
public static const uint kGuideMotion = 4;  // xy pixels to the previous frame, z view depth change
public static const uint kGuideSurface = 5; // RGBA32_UINT packed RTXDI surface (viewDepth, oct n, oct geoN, f0+rough)
public static const uint kGuideImageCount = 6;

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
    public uint directRadianceStorage; // RGBA16F, rtxdi_shade; no emissive
    public uint directRadianceSampled;
    public uint indirectRadianceStorage; // RGBA16F, rtxdi_gi_shade: ReSTIR GI's indirect light
    public uint indirectRadianceSampled;
    public uint noisyColorStorage; // RGBA16F, combine: direct + emissive, the DLSS-RR color input
    public uint noisyColorSampled;
    public uint presentSampled; // present's source: the post target for Lit, else the display image
    public uint upscaledSampled; // RGBA16F DLSS-RR output, linear HDR
    public uint postStorage;     // RGBA16F, display resolution: tonemapped, linear [0, 1]
    public uint exposureStorage; // R32F 1x1: the exposure scale for adaptation and tonemapping
    public uint2 displaySize;
    public uint binIdsStorage; // R8_UINT kMaterialBin* per pixel, written by classify
    public uint binIdsSampled;
    public uint giStagnancyStorage;  // R32F smoothed ReSTIR GI stagnancy, this frame's; written by rtxdi_gi_shade
    public uint giStagnancyPrevious; // last frame's, sampled
    public uint giSampleIdsStorage;   // R32_UINT final ReSTIR GI sample per pixel, 0: none; rtxdi_gi_shade
    public uint giDuplicationStorage; // R32F neighbors sharing it, rtxdi_gi_duplication; next frame's temporal reads
    public uint glassDiffuseAlbedoStorage; // RGBA8 DLSS-RR guides blended with glass (m8 I13 §4 variant b)
    public uint glassSpecAlbedoStorage;
    public uint specularHitDistanceStorage; // R16F DLSS-RR specular hit distance (kNoneIndex when unused, m8 I16)
    public uint dlssInputAccumulationStorage; // RGBA32F DLSS-RR input sum and count, combine (settings lab, m8 I16)
    public uint rawIndirectWeightStorage;     // RGBA16F primary weight + x2 traced luminance (m8.3 RawPt + NRC)
};

// exposure histogram: bin 0 includes black, bin 255 is the ceiling
public static const uint kExposureHistogramBins = 256u;
public static const float kExposureMinLog2 = -12.0;
public static const float kExposureLog2Range = 24.0;

// 16 B, matches src/core/types.hpp
public struct GpuExposureReadback
{
    public float exposure;         // scale applied before the tonemap
    public float targetExposure;   // where the adaptation is heading
    public float averageLuminance; // trimmed geometric mean; 0 when no pixel was metered
    public uint pixels;            // finite metered pixels
};

// [InstanceIndex()] of a TLAS instance; written by the CPU per scene layout (RayTracingScene)
public struct GpuRayInstance
{
    public uint nodeInstanceIndex; // row in GpuFrameRoot.nodeInstances
    public uint primitiveRow;      // row in GpuFrameRoot.primitives
};

// high bit of rayInstanceLights[rayInstance]: sparse remap offset into rayInstanceLights tail (m8 I14 F27)
public static const uint kRayInstanceLightSparseBit = 0x80000000u;

// one active triangle light for light_world (m8 I14 F27); matches src/core/types.hpp
public struct GpuTriangleLightRef
{
    public uint rayInstance;
    public uint localTriangle;
};

// parallel to GpuRayInstance, per scene layout: the PTLAS instance write's inputs beyond the node instance
public struct GpuRayInstanceBuild
{
    public uint64_t blas;
    public uint partition; // grid partition, fixed per layout
    public uint flags;     // kRayInstanceBuildFlag* bits
};

// GpuRayInstanceBuild.flags, matches GpuRayInstanceBuildFlag in src/core/types.hpp
public static const uint kRayInstanceBuildFlagOpacityMicromap = 1u << 0;
public static const uint kRayInstanceBuildFlagGlass = 1u << 1;

// TLAS instance masks (m8 I13): every ray but the glass pass's layer walk and the shadow transmittance traces
// kRayMaskOpaque, so glass blocks nothing and the vis buffer and the primary rays agree
public static const uint kRayMaskOpaque = 0x01u;
public static const uint kRayMaskGlass = 0x02u;

// GpuFrameRoot.frameFlags, matches FrameFlag in src/core/types.hpp
public static const uint kFrameFlagHistoryReset = 1u << 0;
public static const uint kFrameFlagIndirectHistoryReset = 1u << 1;
public static const uint kFrameFlagTriangleLightsUnsampled = 1u << 2;
public static const uint kFrameFlagCleanDlssInput = 1u << 3;
public static const uint kFrameFlagInvertDlssNormal = 1u << 4;
public static const uint kFrameFlagSquareDlssRoughness = 1u << 5;
public static const uint kFrameFlagDlssInputDirectOnly = 1u << 6;
public static const uint kFrameFlagDlssInputIndirectOnly = 1u << 7;
public static const uint kFrameFlagDlssInputAccumulate = 1u << 8;
public static const uint kFrameFlagDlssInputFrozen = 1u << 9;
public static const uint kFrameFlagPreDenoiserClampNeighbors = 1u << 10;
public static const uint kFrameFlagDlssHistoryValid = 1u << 11;
public static const uint kFrameFlagDirectDisabled = 1u << 12;
public static const uint kFrameFlagIndirectDisabled = 1u << 13;
public static const uint kFrameFlagRawIndirect = 1u << 14;

// per-frame root, matches GpuFrameRoot in src/core/types.hpp
public struct GpuFrameRoot
{
    public uint64_t camera;
    public uint64_t instances;     // GpuObjectUB[entity]
    public uint64_t models;        // GpuModel[model]
    public uint64_t primitives;    // GpuPrimitive[], absolute rows
    public uint64_t materials;     // whole GpuMaterial[]; only row 0 (default) is read here
    public uint64_t pbrExtensions; // whole GpuMaterialExtension[], parallel to materials
    public uint64_t lights;        // GpuLight[lightCount], model space; point and spot before directional
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
    public uint localLightCount;  // RTXDI's local region: point and spot rows, then the triangle lights
    public uint64_t rayInstances;   // GpuRayInstance[], 0 until the RT scene exists
    public uint64_t rtxdiConstants; // 0 until M3
    public uint64_t cacheConstants; // GpuNrcConstants of this frame slot (M4 G3)
    public uint frameIndex;
    public float exposure;          // 1: scene-linear color uses unity pre-exposure (considerations C-H2-1)
    public uint64_t worldLights;    // GpuLight[lightCount], world space, written this frame (loadWorldLight)
    public uint64_t prevWorldLights; // last frame's worldLights, same rows; 0 on a history reset
    public uint64_t rayDebug;        // GpuRayDebugCounters of this frame slot; 0 unless kDebugRays
    public uint64_t materialBins;    // GpuMaterialBins, then the per-bin tile lists
    public uint64_t rayInstanceLights; // uint[], parallel to rayInstances: first triangle-region row or kNoneIndex
    public uint firstTriangleLight;    // world row of triangle light 0, after the point and spot rows
    public uint triangleLightCount;
    public uint glassFlags;            // kGlassFlag* (m8 I13); 0 without glass instances
    public uint glassMaxLayers;        // layers a glass walk crosses at most
    public uint64_t glassCounters;     // GpuGlassCounters of this frame slot
    public float preDenoiserClamp;     // > 0: combine clamps the DLSS-RR input to this multiple of a reference (T11)
    public float inputHistoryWeight;   // > 0: combine blends the DLSS-RR input into its running average (lab, m8 I16)
    public uint64_t sharcConstants;    // GpuSharcConstants of this frame slot (M8.4); 0 when SHaRC is inactive
};

// GpuFrameRoot.glassFlags, matches GlassFlag in src/core/types.hpp
public static const uint kGlassFlagPass = 1u << 0;
public static const uint kGlassFlagShadowTransmittance = 1u << 1;
public static const uint kGlassFlagReflection = 1u << 2;
public static const uint kGlassFlagDiffuseShare = 1u << 3;
public static const uint kGlassFlagEmission = 1u << 4;
public static const uint kGlassFlagBlendGuides = 1u << 5;
// matches kGlassMaxLayersLimit in src/core/types.hpp
public static const uint kGlassMaxLayersLimit = 8;

// matches GpuGlassCounters in src/core/types.hpp
public struct GpuGlassCounters
{
    public uint pixels;
    public uint earlyStops;
    public uint capped;
    public uint pad0;
    public uint layerPixels[kGlassMaxLayersLimit];
};
