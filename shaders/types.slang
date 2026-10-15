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
    uint64_t materials;
    uint64_t pbrExtensions;
    uint64_t normals;
    uint64_t tangents;
    uint64_t uv1;
    uint64_t joints;
    uint64_t weights;
    uint64_t indices;
    uint attributeMask;
    uint pad0;
};

// values are relative to the owning model's slices
struct GpuPrimitive
{
    uint firstMeshlet;
    uint meshletCount;
    uint materialId; // kNoneIndex = default material (row 0 of GpuFrameRoot.materials)
    uint firstVertex;
    uint vertexCount;
    uint firstIndex;
    uint indexCount;
    uint pad0;
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

struct GpuLight
{
    float3 worldPos;
    float range;
    float3 worldDir;
    float intensity;
    float3 color;
    uint type;
    float innerCone;
    float outerCone;
    float pad0;
    float pad1;
};

// MeshPushData: 24 B, matches src/render/push_data.hpp
struct MeshPushData
{
    uint64_t frameRoot;
    uint instanceIndex;
    uint primitiveIndex;
    uint meshletOffset; // first meshlet of this batch, relative to the primitive
    uint meshletCount;
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
    uint64_t models;        // GpuModel[entity]
    uint64_t primitives;    // GpuPrimitive[], absolute rows
    uint64_t materials;     // GpuMaterial[], absolute rows
    uint64_t pbrExtensions;
    uint64_t lights;        // GpuLight[lightCount]
    uint64_t tlas;
    uint lightCount;
    uint instanceCount;
    uint primitiveCount;
    uint pad0;
};

#endif
