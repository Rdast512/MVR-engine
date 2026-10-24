#ifndef ENGINE_TYPES_SLANG
#define ENGINE_TYPES_SLANG

// scalar layout: must match src/core/types.hpp + MeshPushData (slangc -fvk-use-scalar-layout)

static const uint kNoneIndex = 0xffffffffu;
static const uint kMaxMeshletVertices = 64;
static const uint kMaxMeshletTriangles = 126;
static const uint kGroupSize = 32;

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
};

// one entity x node, transforms precomputed per frame
struct GpuNodeInstance
{
    float4x4 worldMatrix;
    float4x4 prevWorldMatrix;
    float4 boundingSphere; // world space
    uint entityIndex;
    uint modelIndex;
    uint nodeIndex;
    uint flags;
};

struct GpuDrawRecord
{
    uint nodeInstanceIndex;
    uint primitiveIndex; // absolute row of GpuFrameRoot.primitives
    uint meshletOffset;  // relative to the primitive
    uint pad0;
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
    uint vertexCount;
    uint triangleCount;
    float4 boundingSphere;
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

// MeshPushData: 24 B, matches src/render/push_data.hpp
struct MeshPushData
{
    uint64_t frameRoot;
    uint drawIndex;    // row of GpuFrameRoot.drawRecords
    uint meshletCount; // groups in this batch
};

struct MeshVertexOut
{
    float4 pos          : SV_Position;
    float3 fragColor    : COLOR;
    float2 fragTexCoord : TEXCOORD;
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
    uint64_t drawRecords;   // GpuDrawRecord[drawRecordCount]
    uint lightCount;
    uint instanceCount;
    uint primitiveCount;
    uint nodeInstanceCount;
    uint drawRecordCount;
    uint debugView; // kDebugView* mode in the low byte, kDebugViewFlag* above
};

#endif
