#pragma once


struct TextureAsset {
    vk::raii::Image textureImage = nullptr;
    vk::raii::ImageView textureImageView = nullptr;;
    VmaAllocation textureImageMemory = nullptr;
    uint32_t descriptorHeapIndex;
};


// Tight packing: pos@0, color@12, texCoord@24, sizeof==32 (glm). Mesh BDA path requires
// device scalarBlockLayout + slangc -fvk-use-scalar-layout so SPIR-V matches this layout.
struct Vertex
{
    glm::vec3 pos;
    glm::vec3 color;
    glm::vec2 texCoord;

    bool operator==(const Vertex& other) const
    {
        return pos == other.pos && color == other.color && texCoord == other.texCoord;
    }
};
static_assert(sizeof(Vertex) == 32, "Vertex must stay 32 B for mesh BDA / scalar layout");
static_assert(offsetof(Vertex, color) == 12);
static_assert(offsetof(Vertex, texCoord) == 24);

namespace std
{
    template <>
    struct hash<Vertex>
    {
        size_t operator()(Vertex const& vertex) const
        {
            return ((hash<glm::vec3>()(vertex.pos) ^ (hash<glm::vec3>()(vertex.color) << 1)) >> 1) ^
                (hash<glm::vec2>()(vertex.texCoord) << 1);
        }
    };
} // namespace std


// GPU-friendly meshlet header (CPU layout matches mesh.slang MeshletDesc / SSBO).
struct alignas(16) MeshletDesc
{
    uint32_t vertexOffset = 0; // into meshletVertices
    uint32_t triangleOffset = 0; // into meshletTriangles (first corner index)
    uint32_t vertexCount = 0;
    uint32_t triangleCount = 0;
    glm::vec4 boundingSphere{0.0f}; // xyz = center, w = radius (object space)
};
static_assert(sizeof(MeshletDesc) == 32, "MeshletDesc must match mesh.slang (4x uint + float4, 32 B)");

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

struct UniformBufferObject
{
    alignas(16) glm::mat4 model;
    alignas(16) glm::mat4 view;
    alignas(16) glm::mat4 proj;
};


struct alignas(16) CameraData {
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

struct alignas(16) ObjectUB {
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
