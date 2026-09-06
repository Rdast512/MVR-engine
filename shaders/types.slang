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

    uint materialID;
    uint instanceFlags;
    uint baseVertex;
    uint baseIndex;
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

// MeshPushData: 72 B, matches src/render/push_data.hpp
struct MeshPushData
{
    uint64_t cameraAddress;
    uint64_t objectUbAddress;
    uint64_t vertices;
    uint64_t meshlets;
    uint64_t meshletVertices;
    uint64_t meshletTriangles;
    uint firstMeshlet;
    uint meshletCount;
    DescriptorHandle<Texture2D> texture;
    DescriptorHandle<SamplerState> samplerHandle;
};

struct MeshVertexOut
{
    float4 pos          : SV_Position;
    float3 fragColor    : COLOR;
    float2 fragTexCoord : TEXCOORD;
};

// future frame table (BDA + heap ids). raster still uses MeshPushData.
struct FrameRoot
{
    uint64_t cameraAddress;
    uint64_t objectUbAddress;
    uint64_t vertices;
    uint64_t meshlets;
    uint64_t meshletVertices;
    uint64_t meshletTriangles;
    uint64_t materials;
    uint64_t pbrExt;
    uint64_t lights;
    uint64_t tlas;
};

#endif
