#pragma once

#include <string>
#include <utility>
#include <vector>

struct TextureAsset {
    vk::raii::Image textureImage = nullptr;
    vk::raii::ImageView textureImageView = nullptr;;
    VmaAllocation textureImageMemory = nullptr;
    uint32_t descriptorHeapIndex;
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

namespace std
{
    template <>
    struct hash<GpuVertex>
    {
        size_t operator()(GpuVertex const& vertex) const
        {
            return ((hash<glm::vec3>()(vertex.pos) ^ (hash<glm::vec3>()(vertex.color) << 1)) >> 1) ^
                (hash<glm::vec2>()(vertex.texCoord) << 1);
        }
    };
} // namespace std


// GPU-friendly meshlet header (CPU layout matches mesh.slang GpuMeshletDesc / SSBO).
struct alignas(16) GpuMeshletDesc
{
    uint32_t vertexOffset = 0; // into meshletVertices
    uint32_t triangleOffset = 0; // into meshletTriangles (first corner index)
    uint32_t vertexCount = 0;
    uint32_t triangleCount = 0;
    glm::vec4 boundingSphere{0.0f}; // xyz = center, w = radius (object space)
};
static_assert(sizeof(GpuMeshletDesc) == 32, "GpuMeshletDesc must match mesh.slang (4x uint + float4, 32 B)");

// Per-entity range into the global meshlet arrays.
struct MeshletDraw
{
    uint32_t firstMeshlet = 0;
    uint32_t meshletCount = 0;
};

struct MaterialRef
{
    uint32_t textureIndex = 0;
    uint32_t materialId = 0;
};

inline constexpr uint32_t kNoneIndex = ~0u;

struct PrimitiveDraw
{
    MeshletDraw meshlets;
    uint32_t materialId = kNoneIndex;
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    uint32_t morphFirst = 0;
    uint32_t morphCount = 0;
    uint32_t morphWeightFirst = 0;
};

struct MorphTarget
{
    uint32_t posOffset = kNoneIndex;
    uint32_t nrmOffset = kNoneIndex;
    uint32_t tanOffset = kNoneIndex;
};

namespace GpuMaterialFlag
{
    inline constexpr uint32_t AlphaOpaque = 0;
    inline constexpr uint32_t AlphaMask = 1;
    inline constexpr uint32_t AlphaBlend = 2;
    inline constexpr uint32_t AlphaModeMask = 3u;
    inline constexpr uint32_t DoubleSided = 1u << 2;
} // namespace GpuMaterialFlag

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
};

struct SamplerDesc
{
    int32_t minFilter = -1;
    int32_t magFilter = -1;
    int32_t wrapS = 10497;
    int32_t wrapT = 10497;
    uint32_t heapIndex = 0;
};

struct LightDef
{
    uint8_t type = 1;
    glm::vec3 color{1.0f};
    float intensity = 1.0f;
    float range = 0.0f;
    float innerCone = 0.0f;
    float outerCone = 0.785398163f;
};

struct LightInstance
{
    uint32_t defIndex = kNoneIndex;
    glm::vec3 worldPos{0.0f};
    glm::vec3 worldDir{0.0f, 0.0f, -1.0f};
};

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

struct GpuUniformBufferObject
{
    alignas(16) glm::mat4 model;
    alignas(16) glm::mat4 view;
    alignas(16) glm::mat4 proj;
};


struct alignas(16) GpuCameraData {
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
    glm::vec3 cameraPos;       // World-space camera position (for specular/lighting calculations)
    float nearZ;               // Near clipping plane

    glm::vec2 renderTargetSize;// Width, Height in pixels
    glm::vec2 invRenderTargetSize; // 1.0 / Width, 1.0 / Height

    glm::vec2 jitterOffset;    // TAA subpixel jitter offset
    float farZ;                // Far clipping plane
    float frameDeltaTime;      // Delta time in seconds
    glm::vec4 cameraParams; // reserved
};

struct alignas(16) GpuObjectUB {
    // Transform Matrices
    glm::mat4 modelMatrix;     // World transformation matrix (64 bytes)
    glm::mat4 prevModelMatrix; // Previous frame world matrix for temporal motion vectors (64 bytes)

    // // Direct BDA Geometry Pointers
    // uint64_t vertexBufferAddress; // GPU Virtual Address of vertex array (8 bytes)
    // uint64_t indexBufferAddress;  // GPU Virtual Address of index array (8 bytes)

    // Bounding Box / Sphere for GPU Culling (Frustum & Occlusion)
    glm::vec4 boundingSphere;     // xyz = center, w = radius (16 bytes)

    // Resource & Material Handles
    uint32_t materialID;          // Index into global Material SSBO array (4 bytes)
    uint32_t instanceFlags;        // Bit flags (e.g., bit 0: dynamic, bit 1: cast shadow) (4 bytes)
    uint32_t baseVertex;          // Vertex offset in buffer (4 bytes)
    uint32_t baseIndex;           // Index offset in buffer (4 bytes)
};


struct EngineSettings
{
    uint8_t xResolution;
    uint8_t yResolution;
    uint8_t mipmapLevel;
    bool hdr;
    bool fullscreen;
    bool vsync;
    bool debug;
    bool windowed;
};

enum class DescriptorBindingMode : uint8_t
{
    LegacySets = 0,
    DescriptorHeaps = 1,
};

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
    vk::PhysicalDeviceDescriptorBufferPropertiesEXT descriptorBuffer;

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
    vk::PhysicalDeviceClusterAccelerationStructurePropertiesNV clusterAccelerationStructure;
    vk::PhysicalDevicePartitionedAccelerationStructurePropertiesNV partitionedAccelerationStructure;
};

struct EngineContext
{
    EngineSettings settings;
    HardwareCapabilities hardwareCapabilities;
};
