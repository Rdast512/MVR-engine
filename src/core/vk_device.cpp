/**
 * @file vk_device.cpp
 * @brief Vulkan device management: instance, physical-device selection, logical device, and queue setup.
 *
 * This translation unit implements the full Vulkan device initialisation pipeline:
 *   - Vulkan instance creation with SDL3 WSI extensions and optional validation
 *   - VK_EXT_debug_utils messenger registration for validation layer output
 *   - Window-surface creation via SDL3
 *   - Physical-device selection: required extensions/features, then scoring
 *   - Logical-device creation with an exhaustive feature chain (Vulkan 1.1–1.4 +
 *     ray-tracing, mesh shaders, shader objects, present-timing, etc.)
 *   - Queue family resolution with fallback strategies for transfer and compute
 */
#include "vk_device.hpp"
#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string_view>
#include "../static_headers/logger.hpp"
#include "../util/debug.hpp"
#include "vulkan/vulkan.hpp"

namespace
{
// Requested when enableValidationLayers is true.
constexpr std::array kValidationLayers = {"VK_LAYER_KHRONOS_validation"};

// Promoted-to-core extensions are enabled through Vulkan1xFeatures and must not be listed here,
// see docs/vulkan_extensions_reference.md "Promoted-to-Core Features".
// Optional extensions (NV) are appended in createLogicalDevice when supported.
constexpr std::array kRequiredDeviceExtensions = {
    // KHR
    vk::KHRSwapchainExtensionName,
    vk::KHRMaintenance7ExtensionName,
    vk::KHRMaintenance8ExtensionName,
    vk::KHRMaintenance9ExtensionName,
    vk::KHRMaintenance10ExtensionName,
    vk::KHRDeferredHostOperationsExtensionName, // required by KHR_acceleration_structure
    vk::KHRAccelerationStructureExtensionName,
    vk::KHRRayTracingPipelineExtensionName,
    vk::KHRFragmentShadingRateExtensionName,
    vk::KHRRayQueryExtensionName,
    vk::KHRSwapchainMaintenance1ExtensionName,
    vk::KHRRayTracingMaintenance1ExtensionName,
    vk::KHRPresentId2ExtensionName, // required by EXT_present_timing
    vk::KHRCalibratedTimestampsExtensionName, // required by EXT_present_timing
    vk::KHRPresentModeFifoLatestReadyExtensionName,
    vk::KHRCopyMemoryIndirectExtensionName,
    vk::KHRShaderUntypedPointersExtensionName,
    vk::KHRDeviceAddressCommandsExtensionName,
    vk::KHRRobustness2ExtensionName, // shader objects take no VkPipelineRobustnessCreateInfo
    // EXT
    vk::EXTOpacityMicromapExtensionName,
    vk::EXTMemoryBudgetExtensionName,
    vk::EXTMemoryPriorityExtensionName,
    vk::EXTMemoryDecompressionExtensionName,
    vk::EXTDescriptorHeapExtensionName,
    vk::EXTBlendOperationAdvancedExtensionName,
    vk::EXTMeshShaderExtensionName,
    vk::EXTDeviceGeneratedCommandsExtensionName,
    vk::EXTPageableDeviceLocalMemoryExtensionName,
    vk::EXTShaderObjectExtensionName,
    vk::EXTPresentTimingExtensionName,
    vk::EXTRayTracingInvocationReorderExtensionName,
    vk::EXTExtendedDynamicState3ExtensionName,
};

// NRC tier; without them the GI cache falls back to SHaRC
constexpr std::array kNrcDeviceExtensions = {
    vk::NVXBinaryImportExtensionName,
    vk::NVXImageViewHandleExtensionName,
};

// a VkBool32 feature field, located by its struct's sType and byte offset so one table
// both enables the feature and names it when the GPU lacks it
struct FeatureBit
{
    vk::StructureType structType;
    size_t offset;
    std::string_view name;
};

#define ENGINE_FEATURE(Struct, field)                                                                                  \
    FeatureBit                                                                                                         \
    {                                                                                                                  \
        vk::Struct::structureType, offsetof(vk::Struct, field), #Struct "::" #field                                    \
    }
#define ENGINE_CORE_FEATURE(field)                                                                                     \
    FeatureBit                                                                                                         \
    {                                                                                                                  \
        vk::PhysicalDeviceFeatures2::structureType,                                                                    \
            offsetof(vk::PhysicalDeviceFeatures2, features) + offsetof(vk::PhysicalDeviceFeatures, field),             \
            "PhysicalDeviceFeatures::" #field                                                                          \
    }

// device creation fails without any of these
constexpr std::array kRequiredFeatures = {
    // required by robustBufferAccess2
    ENGINE_CORE_FEATURE(robustBufferAccess),
    ENGINE_CORE_FEATURE(geometryShader),
    ENGINE_CORE_FEATURE(sampleRateShading),
    ENGINE_CORE_FEATURE(multiDrawIndirect),
    ENGINE_CORE_FEATURE(samplerAnisotropy),
    // BC7 KTX2 textures from tools/gltf_ktx2
    ENGINE_CORE_FEATURE(textureCompressionBC),
    ENGINE_CORE_FEATURE(shaderInt64),
    // mesh.slang: uint8_t* meshletTriangles via PhysicalStorageBuffer
    ENGINE_FEATURE(PhysicalDeviceVulkan12Features, storageBuffer8BitAccess),
    ENGINE_FEATURE(PhysicalDeviceVulkan12Features, shaderInt8),
    ENGINE_FEATURE(PhysicalDeviceVulkan12Features, descriptorIndexing),
    ENGINE_FEATURE(PhysicalDeviceVulkan12Features, shaderSampledImageArrayNonUniformIndexing),
    ENGINE_FEATURE(PhysicalDeviceVulkan12Features, shaderStorageBufferArrayNonUniformIndexing),
    ENGINE_FEATURE(PhysicalDeviceVulkan12Features, descriptorBindingPartiallyBound),
    ENGINE_FEATURE(PhysicalDeviceVulkan12Features, descriptorBindingVariableDescriptorCount),
    ENGINE_FEATURE(PhysicalDeviceVulkan12Features, runtimeDescriptorArray),
    // GpuVertex float3@0/12 packing with slang -fvk-use-scalar-layout
    ENGINE_FEATURE(PhysicalDeviceVulkan12Features, scalarBlockLayout),
    ENGINE_FEATURE(PhysicalDeviceVulkan12Features, bufferDeviceAddress),
    ENGINE_FEATURE(PhysicalDeviceVulkan12Features, vulkanMemoryModel),
    ENGINE_FEATURE(PhysicalDeviceVulkan12Features, vulkanMemoryModelDeviceScope),
    // hi-z max-reduction sampler
    ENGINE_FEATURE(PhysicalDeviceVulkan12Features, samplerFilterMinmax),
    // textureCompressionASTC_HDR stays off: rtx 2060 super not supported
    ENGINE_FEATURE(PhysicalDeviceVulkan13Features, synchronization2),
    ENGINE_FEATURE(PhysicalDeviceVulkan13Features, dynamicRendering),
    ENGINE_FEATURE(PhysicalDeviceVulkan13Features, maintenance4),
    ENGINE_FEATURE(PhysicalDeviceVulkan14Features, globalPriorityQuery),
    ENGINE_FEATURE(PhysicalDeviceVulkan14Features, dynamicRenderingLocalRead),
    ENGINE_FEATURE(PhysicalDeviceVulkan14Features, maintenance5),
    ENGINE_FEATURE(PhysicalDeviceVulkan14Features, maintenance6),
    ENGINE_FEATURE(PhysicalDeviceVulkan14Features, hostImageCopy),
    ENGINE_FEATURE(PhysicalDeviceExtendedDynamicStateFeaturesEXT, extendedDynamicState),
    // logicOp / patchControlPoints are not promoted and unused (mesh-only, no logic op)
    ENGINE_FEATURE(PhysicalDeviceExtendedDynamicState2FeaturesEXT, extendedDynamicState2),
    // states set by ShaderObjects::bindMeshState
    ENGINE_FEATURE(PhysicalDeviceExtendedDynamicState3FeaturesEXT, extendedDynamicState3PolygonMode),
    ENGINE_FEATURE(PhysicalDeviceExtendedDynamicState3FeaturesEXT, extendedDynamicState3RasterizationSamples),
    ENGINE_FEATURE(PhysicalDeviceExtendedDynamicState3FeaturesEXT, extendedDynamicState3SampleMask),
    ENGINE_FEATURE(PhysicalDeviceExtendedDynamicState3FeaturesEXT, extendedDynamicState3AlphaToCoverageEnable),
    ENGINE_FEATURE(PhysicalDeviceExtendedDynamicState3FeaturesEXT, extendedDynamicState3ColorBlendEnable),
    ENGINE_FEATURE(PhysicalDeviceExtendedDynamicState3FeaturesEXT, extendedDynamicState3ColorWriteMask),
    ENGINE_FEATURE(PhysicalDeviceDescriptorHeapFeaturesEXT, descriptorHeap),
    // advancedBlendCoherentOperations stays off
    ENGINE_FEATURE(PhysicalDeviceMeshShaderFeaturesEXT, taskShader),
    ENGINE_FEATURE(PhysicalDeviceMeshShaderFeaturesEXT, meshShader),
    ENGINE_FEATURE(PhysicalDeviceMeshShaderFeaturesEXT, meshShaderQueries),
    ENGINE_FEATURE(PhysicalDeviceDeviceGeneratedCommandsFeaturesEXT, deviceGeneratedCommands),
    ENGINE_FEATURE(PhysicalDeviceMemoryPriorityFeaturesEXT, memoryPriority),
    ENGINE_FEATURE(PhysicalDeviceMemoryDecompressionFeaturesEXT, memoryDecompression),
    ENGINE_FEATURE(PhysicalDevicePageableDeviceLocalMemoryFeaturesEXT, pageableDeviceLocalMemory),
    // PresentTimingFeaturesEXT: extension enabled, feature not requested yet
    ENGINE_FEATURE(PhysicalDeviceRayTracingInvocationReorderFeaturesEXT, rayTracingInvocationReorder),
    ENGINE_FEATURE(PhysicalDeviceOpacityMicromapFeaturesEXT, micromap),
    ENGINE_FEATURE(PhysicalDeviceShaderObjectFeaturesEXT, shaderObject),
    ENGINE_FEATURE(PhysicalDeviceFragmentShadingRateFeaturesKHR, pipelineFragmentShadingRate),
    ENGINE_FEATURE(PhysicalDeviceFragmentShadingRateFeaturesKHR, primitiveFragmentShadingRate),
    ENGINE_FEATURE(PhysicalDeviceFragmentShadingRateFeaturesKHR, attachmentFragmentShadingRate),
    ENGINE_FEATURE(PhysicalDeviceDeviceAddressCommandsFeaturesKHR, deviceAddressCommands),
    ENGINE_FEATURE(PhysicalDeviceAccelerationStructureFeaturesKHR, accelerationStructure),
    ENGINE_FEATURE(PhysicalDeviceRayTracingPipelineFeaturesKHR, rayTracingPipeline),
    ENGINE_FEATURE(PhysicalDeviceRayTracingPipelineFeaturesKHR, rayTracingPipelineTraceRaysIndirect),
    ENGINE_FEATURE(PhysicalDeviceRayTracingPipelineFeaturesKHR, rayTraversalPrimitiveCulling),
    ENGINE_FEATURE(PhysicalDeviceRayQueryFeaturesKHR, rayQuery),
    ENGINE_FEATURE(PhysicalDeviceRayTracingMaintenance1FeaturesKHR, rayTracingMaintenance1),
    ENGINE_FEATURE(PhysicalDeviceRayTracingMaintenance1FeaturesKHR, rayTracingPipelineTraceRaysIndirect2),
    ENGINE_FEATURE(PhysicalDeviceSwapchainMaintenance1FeaturesKHR, swapchainMaintenance1),
    ENGINE_FEATURE(PhysicalDeviceMaintenance7FeaturesKHR, maintenance7),
    ENGINE_FEATURE(PhysicalDeviceMaintenance8FeaturesKHR, maintenance8),
    // maintenance9 stays off while using Khronos Best Practices (VVL #12449, SDK ≤1.4.357).
    // BP ValidateImageInQueue does: qf_count = last_usage.queue_family_index + 1
    // then qf_props.back(). On first image use last_usage is still
    // VK_QUEUE_FAMILY_IGNORED (0xFFFFFFFF) → count wraps to 0 → empty
    // vector .back() writes to 0xFFFFFFFFFFFFFFE0 (sizeof(VkQueueFamilyProperties2)
    // element -1, pNext at +8). Submit queue family from getDeviceQueue is fine;
    // IGNORED is VVL's "never used" sentinel, not an app QFOT barrier mistake.
    // Re-enable when VVL skips IGNORED/UNDEFINED before that query.
    ENGINE_FEATURE(PhysicalDeviceMaintenance10FeaturesKHR, maintenance10),
    ENGINE_FEATURE(PhysicalDeviceCopyMemoryIndirectFeaturesKHR, indirectMemoryCopy),
    ENGINE_FEATURE(PhysicalDeviceCopyMemoryIndirectFeaturesKHR, indirectMemoryToImageCopy),
    ENGINE_FEATURE(PhysicalDevicePresentModeFifoLatestReadyFeaturesKHR, presentModeFifoLatestReady),
    ENGINE_FEATURE(PhysicalDeviceShaderUntypedPointersFeaturesKHR, shaderUntypedPointers),
    ENGINE_FEATURE(PhysicalDeviceRobustness2FeaturesKHR, robustBufferAccess2),
    ENGINE_FEATURE(PhysicalDeviceRobustness2FeaturesKHR, robustImageAccess2),
    ENGINE_FEATURE(PhysicalDeviceRobustness2FeaturesKHR, nullDescriptor),
};

// enabled only with their NV extensions
constexpr std::array kClusterAccelerationStructureFeatures = {
    ENGINE_FEATURE(PhysicalDeviceClusterAccelerationStructureFeaturesNV, clusterAccelerationStructure),
};
constexpr std::array kPartitionedAccelerationStructureFeatures = {
    ENGINE_FEATURE(PhysicalDevicePartitionedAccelerationStructureFeaturesNV, partitionedAccelerationStructure),
};

// NRC guide step 2.2; scalarBlockLayout, bufferDeviceAddress and memory budget are already required
constexpr std::array kNrcFeatures = {
    ENGINE_FEATURE(PhysicalDeviceVulkan12Features, uniformBufferStandardLayout),
};

// SHaRC hash insertion uses 64-bit atomics, resolved entries store float16_t radiance
constexpr std::array kSharcFeatures = {
    ENGINE_FEATURE(PhysicalDeviceVulkan12Features, shaderBufferInt64Atomics),
    ENGINE_FEATURE(PhysicalDeviceVulkan12Features, shaderFloat16),
};

#undef ENGINE_FEATURE
#undef ENGINE_CORE_FEATURE

using FeatureChain = vk::StructureChain<
    vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan11Features, vk::PhysicalDeviceVulkan12Features,
    vk::PhysicalDeviceVulkan13Features, vk::PhysicalDeviceVulkan14Features,
    // EXT
    vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT, vk::PhysicalDeviceExtendedDynamicState2FeaturesEXT,
    vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT, vk::PhysicalDeviceDescriptorHeapFeaturesEXT,
    vk::PhysicalDeviceBlendOperationAdvancedFeaturesEXT, vk::PhysicalDeviceMeshShaderFeaturesEXT,
    vk::PhysicalDeviceDeviceGeneratedCommandsFeaturesEXT, vk::PhysicalDeviceMemoryPriorityFeaturesEXT,
    vk::PhysicalDeviceMemoryDecompressionFeaturesEXT, vk::PhysicalDevicePageableDeviceLocalMemoryFeaturesEXT,
    vk::PhysicalDevicePresentTimingFeaturesEXT, vk::PhysicalDeviceRayTracingInvocationReorderFeaturesEXT,
    vk::PhysicalDeviceOpacityMicromapFeaturesEXT, vk::PhysicalDeviceShaderObjectFeaturesEXT,
    // KHR
    vk::PhysicalDeviceFragmentShadingRateFeaturesKHR, vk::PhysicalDeviceDeviceAddressCommandsFeaturesKHR,
    vk::PhysicalDeviceAccelerationStructureFeaturesKHR, vk::PhysicalDeviceRayTracingPipelineFeaturesKHR,
    vk::PhysicalDeviceRayQueryFeaturesKHR, vk::PhysicalDeviceRayTracingMaintenance1FeaturesKHR,
    vk::PhysicalDeviceSwapchainMaintenance1FeaturesKHR, vk::PhysicalDeviceMaintenance7FeaturesKHR,
    vk::PhysicalDeviceMaintenance8FeaturesKHR, vk::PhysicalDeviceMaintenance9FeaturesKHR,
    vk::PhysicalDeviceMaintenance10FeaturesKHR, vk::PhysicalDeviceCopyMemoryIndirectFeaturesKHR,
    vk::PhysicalDevicePresentModeFifoLatestReadyFeaturesKHR, vk::PhysicalDeviceShaderUntypedPointersFeaturesKHR,
    vk::PhysicalDeviceRobustness2FeaturesKHR, vk::PhysicalDeviceClusterAccelerationStructureFeaturesNV,
    vk::PhysicalDevicePartitionedAccelerationStructureFeaturesNV>;

using QueueFamilyChain = vk::StructureChain<vk::QueueFamilyProperties2, vk::QueueFamilyOwnershipTransferPropertiesKHR>;

[[nodiscard]] bool isExtensionSupported(const std::vector<vk::ExtensionProperties>& available, std::string_view name)
{
    return std::ranges::any_of(available, [name](const vk::ExtensionProperties& extension)
                               { return name == extension.extensionName.data(); });
}

void appendMissingExtensions(const std::vector<vk::ExtensionProperties>& available,
                             std::span<const char* const> extensions, std::vector<std::string_view>& missing)
{
    for (const char* extension : extensions) {
        if (!isExtensionSupported(available, extension)) {
            missing.emplace_back(extension);
        }
    }
}

[[nodiscard]] std::string joinNames(std::span<const std::string_view> names)
{
    std::string joined;
    for (const std::string_view name : names) {
        joined += std::format("{}{}", joined.empty() ? "" : ", ", name);
    }
    return joined;
}

// logs every unmet requirement of an init step before failing it
[[noreturn]] void failStep(std::string_view step, std::span<const std::string_view> missing)
{
    const std::string message = std::format("{} failed, missing: {}", step, joinNames(missing));
    logError(message, "Device");
    throw std::runtime_error(message);
}

// structs of unsupported extensions must not be in a feature query or create chain
void unlinkUnsupportedFeatures(FeatureChain& chain, const std::vector<vk::ExtensionProperties>& available)
{
    if (!isExtensionSupported(available, vk::NVClusterAccelerationStructureExtensionName)) {
        chain.unlink<vk::PhysicalDeviceClusterAccelerationStructureFeaturesNV>();
    }
    if (!isExtensionSupported(available, vk::NVPartitionedAccelerationStructureExtensionName)) {
        chain.unlink<vk::PhysicalDevicePartitionedAccelerationStructureFeaturesNV>();
    }
}

// field of bit inside its linked struct; nullptr when that struct is unlinked
[[nodiscard]] vk::Bool32* findFeature(FeatureChain& chain, const FeatureBit& bit)
{
    auto* node = reinterpret_cast<vk::BaseOutStructure*>(&chain.get<vk::PhysicalDeviceFeatures2>());
    for (; node != nullptr; node = node->pNext) {
        if (node->sType == bit.structType) {
            return reinterpret_cast<vk::Bool32*>(reinterpret_cast<std::byte*>(node) + bit.offset);
        }
    }
    return nullptr;
}

void enableFeatures(FeatureChain& chain, std::span<const FeatureBit> bits)
{
    for (const FeatureBit& bit : bits) {
        if (vk::Bool32* field = findFeature(chain, bit)) {
            *field = vk::True;
        }
    }
}

void appendMissingFeatures(FeatureChain& supported, std::span<const FeatureBit> bits,
                           std::vector<std::string_view>& missing)
{
    for (const FeatureBit& bit : bits) {
        const vk::Bool32* field = findFeature(supported, bit);
        if (field == nullptr || *field != vk::True) {
            missing.push_back(bit.name);
        }
    }
}

// caller unlinks structs of absent extensions first
void querySupportedFeatures(const vk::raii::PhysicalDevice& device, FeatureChain& supported)
{
    device.getDispatcher()->vkGetPhysicalDeviceFeatures2(
        static_cast<VkPhysicalDevice>(*device),
        reinterpret_cast<VkPhysicalDeviceFeatures2*>(&supported.get<vk::PhysicalDeviceFeatures2>()));
}

struct GiCacheSupport
{
    GiCache cache = GiCache::None;
    bool hasSharc = false;
};

// NRC first, SHaRC as the fallback tier; logs what each tier is missing
[[nodiscard]] GiCacheSupport selectGiCache(const std::vector<vk::ExtensionProperties>& available,
                                           FeatureChain& supported)
{
    std::vector<std::string_view> nrcMissing;
#if !defined(_WIN32)
    nrcMissing.emplace_back("NRC runtime (Windows-only binaries)");
#endif
    appendMissingExtensions(available, kNrcDeviceExtensions, nrcMissing);
    appendMissingFeatures(supported, kNrcFeatures, nrcMissing);
    std::vector<std::string_view> sharcMissing;
    appendMissingFeatures(supported, kSharcFeatures, sharcMissing);
    const bool hasSharc = sharcMissing.empty();

    if (nrcMissing.empty()) {
        logInfo(std::format("GI cache: NRC{}", hasSharc ? " (SHaRC available alongside)" : ""), "Device");
        return {GiCache::Nrc, hasSharc};
    }
    logInfo(std::format("GI cache: NRC unavailable, missing {}; falling back to SHaRC", joinNames(nrcMissing)),
            "Device");
    if (hasSharc) {
        logInfo("GI cache: SHaRC", "Device");
        return {GiCache::Sharc, true};
    }
    logError(std::format("GI cache disabled: SHaRC missing {}", joinNames(sharcMissing)), "Device");
    return {};
}

// unmet extensions, then unmet features; empty when the GPU is usable
[[nodiscard]] std::vector<std::string_view> missingRequirements(const vk::raii::PhysicalDevice& device)
{
    const std::vector<vk::ExtensionProperties> available = device.enumerateDeviceExtensionProperties();
    std::vector<std::string_view> missing;
    appendMissingExtensions(available, kRequiredDeviceExtensions, missing);
    // feature structs of absent extensions may not be queried
    if (!missing.empty()) {
        return missing;
    }
    FeatureChain supported;
    unlinkUnsupportedFeatures(supported, available);
    querySupportedFeatures(device, supported);
    appendMissingFeatures(supported, kRequiredFeatures, missing);
    return missing;
}

// First family accepted by the predicate (index, flags).
template <typename Predicate>
[[nodiscard]] std::optional<uint32_t> findFamilyIf(std::span<const vk::QueueFamilyProperties> families,
                                                   Predicate accept)
{
    for (uint32_t family = 0; family < families.size(); ++family) {
        if (accept(family, families[family].queueFlags)) {
            return family;
        }
    }
    return std::nullopt;
}

// First family with all required flags and none of the excluded ones.
[[nodiscard]] std::optional<uint32_t> findFamily(std::span<const vk::QueueFamilyProperties> families,
                                                 vk::QueueFlags required, vk::QueueFlags excluded = {})
{
    return findFamilyIf(families, [=](uint32_t, vk::QueueFlags flags)
                        { return (flags & required) == required && !(flags & excluded); });
}

void logQueueFamilies(std::span<const QueueFamilyChain> chains)
{
    for (uint32_t family = 0; family < chains.size(); ++family) {
        const vk::QueueFamilyProperties& properties =
            chains[family].get<vk::QueueFamilyProperties2>().queueFamilyProperties;
        const uint32_t transferTargets =
            chains[family].get<vk::QueueFamilyOwnershipTransferPropertiesKHR>().optimalImageTransferToQueueFamilies;
        std::string targets;
        for (uint32_t target = 0; target < 32; ++target) {
            if (transferTargets & (1u << target)) {
                targets += std::format("{}{}", targets.empty() ? "" : ",", target);
            }
        }
        logInfo(std::format("Queue family {}: count={} flags={} optimalImageTransferTo=[{}]", family,
                             properties.queueCount, vk::to_string(properties.queueFlags), targets),
                 "Device");
    }
}
} // namespace


Device::Device(SDL_Window* window, bool enableValidationLayers) :
    window(window), enableValidationLayers(enableValidationLayers)
{
}

void Device::init()
{
    createInstance();
    setupDebugMessenger();
    createSurface();
    pickPhysicalDevice();
    createLogicalDevice();
}

void Device::createInstance()
{

    constexpr vk::ApplicationInfo appInfo{.pApplicationName = "Hello Triangle",
                                          .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
                                          .pEngineName = "No Engine",
                                          .engineVersion = VK_MAKE_VERSION(1, 0, 0),
                                          .apiVersion = vk::ApiVersion14};

    // WSI extensions required by SDL3
    Uint32 sdlExtensionCount = 0;
    const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&sdlExtensionCount);
    if (sdlExtensions == nullptr) {
        const std::string message = std::format("SDL Vulkan instance extensions unavailable: {}", SDL_GetError());
        logError(message, "Device");
        throw std::runtime_error(message);
    }

    std::vector<const char*> extensions(sdlExtensions, sdlExtensions + sdlExtensionCount);
    extensions.insert(extensions.end(),
                      {
                          // always on, not only with validation: object names and labels for RenderDoc / Nsight
                          vk::EXTDebugUtilsExtensionName,
                          // instance side of KHR_swapchain_maintenance1
                          vk::KHRDisplayExtensionName,
                          vk::KHRSurfaceMaintenance1ExtensionName,
                          vk::KHRGetDisplayProperties2ExtensionName,
                          vk::KHRGetSurfaceCapabilities2ExtensionName,
                      });
    std::string enabledNames;
    for (const char* extension : extensions) {
        enabledNames += std::format("{}{}", enabledNames.empty() ? "" : ", ", extension);
    }
    logInfo(std::format("Enabled instance extensions: {}", enabledNames), "Device");

    std::vector<char const*> requiredLayers;
    if (enableValidationLayers) {
        requiredLayers.assign(kValidationLayers.begin(), kValidationLayers.end());
    }

    // every missing name up front instead of a generic instance-creation error
    std::vector<std::string_view> missing;
    appendMissingExtensions(context.enumerateInstanceExtensionProperties(), extensions, missing);
    const std::vector<vk::LayerProperties> layerProperties = context.enumerateInstanceLayerProperties();
    for (const char* layer : requiredLayers) {
        if (std::ranges::none_of(layerProperties, [layer](const vk::LayerProperties& property)
                                 { return strcmp(property.layerName, layer) == 0; })) {
            missing.emplace_back(layer);
        }
    }
    if (!missing.empty()) {
        failStep("Vulkan instance creation", missing);
    }


    vk::InstanceCreateInfo createInfo{.pApplicationInfo = &appInfo,
                                      .enabledLayerCount = static_cast<uint32_t>(requiredLayers.size()),
                                      .ppEnabledLayerNames = requiredLayers.data(),
                                      .enabledExtensionCount = static_cast<uint32_t>(extensions.size()),
                                      .ppEnabledExtensionNames = extensions.data()};

    try {
        instance = vk::raii::Instance(context, createInfo);
    } catch (const vk::SystemError& err) {
        const std::string message = std::format("Failed to create Vulkan instance: {}", err.what());
        logError(message, "Device");
        throw std::runtime_error(message);
    }
}

/**
 * @brief VK_EXT_debug_utils message callback.
 *
 * Prints all validation/performance/general messages from enabled layers to stderr.
 * Returning VK_FALSE tells the driver that the Vulkan call that triggered the
 * message should NOT be aborted (returning VK_TRUE would only be appropriate for
 * testing layer behaviour itself).
 *
 * @param type      Message category (general / validation / performance).
 * @param pCallbackData  Structured message payload including pMessage string.
 * @return vk::False – do not abort the triggering Vulkan call.
 */
static VKAPI_ATTR vk::Bool32 VKAPI_CALL debugCallback(vk::DebugUtilsMessageSeverityFlagBitsEXT,
                                                      vk::DebugUtilsMessageTypeFlagsEXT type,
                                                      const vk::DebugUtilsMessengerCallbackDataEXT* pCallbackData,
                                                      void*)
{
    std::cerr << "validation layer: type " << to_string(type) << " msg: " << pCallbackData->pMessage << '\n';
    return vk::False;
}


void Device::setupDebugMessenger()
{
    if (!enableValidationLayers)
        return;

    // everything but info severity, all categories
    constexpr vk::DebugUtilsMessageSeverityFlagsEXT severityFlags =
        vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose | vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
        vk::DebugUtilsMessageSeverityFlagBitsEXT::eError;
    constexpr vk::DebugUtilsMessageTypeFlagsEXT messageTypeFlags = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral |
        vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance | vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation;
    const vk::DebugUtilsMessengerCreateInfoEXT createInfo{
        .messageSeverity = severityFlags, .messageType = messageTypeFlags, .pfnUserCallback = &debugCallback};
    debugMessenger = instance.createDebugUtilsMessengerEXT(createInfo);
}


void Device::createSurface()
{
    VkSurfaceKHR rawSurface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(window, *instance, nullptr, &rawSurface)) {
        const std::string message = std::format("Failed to create surface: {}", SDL_GetError());
        logError(message, "Device");
        throw std::runtime_error(message);
    }
    surface = vk::raii::SurfaceKHR(instance, rawSurface);
}


void Device::pickPhysicalDevice()
{
    const auto devices = instance.enumeratePhysicalDevices();

    std::optional<size_t> bestIndex;
    uint32_t bestScore = 0;
    std::string rejections;
    for (size_t i = 0; i < devices.size(); ++i) {
        const vk::PhysicalDeviceProperties properties = devices[i].getProperties2().properties;
        const std::string_view name = properties.deviceName.data();
        if (const std::vector<std::string_view> missing = missingRequirements(devices[i]); !missing.empty()) {
            const std::string rejection = std::format("{}: missing {}", name, joinNames(missing));
            logInfo(std::format("Skipping GPU {}", rejection), "Device");
            rejections += std::format("\n  {}", rejection);
            continue;
        }
        // discrete first; max texture size as a rough capability tiebreak
        const uint32_t score = (properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu ? 1000u : 0u) +
            properties.limits.maxImageDimension2D;
        if (!bestIndex || score > bestScore) {
            bestIndex = i;
            bestScore = score;
        }
    }
    if (!bestIndex) {
        const std::string message = devices.empty()
            ? std::string("No Vulkan physical device found")
            : std::format("No GPU supports the required Vulkan extensions and features:{}", rejections);
        logError(message, "Device");
        throw std::runtime_error(message);
    }
    physicalDevice = devices[*bestIndex];
    logInfo(std::format("Using physical device: {}",
                         std::string_view(physicalDevice.getProperties2().properties.deviceName.data())),
             "Device");
}

void Device::findQueueFamilies(std::span<const vk::QueueFamilyProperties> families)
{
    const auto supportsPresent = [this](uint32_t family)
    { return physicalDevice.getSurfaceSupportKHR(family, *surface) == vk::True; };

    // one family for graphics + present avoids an ownership transfer before present
    const std::optional<uint32_t> graphicsPresent =
        findFamilyIf(families, [&](uint32_t family, vk::QueueFlags flags)
                     { return (flags & vk::QueueFlagBits::eGraphics) && supportsPresent(family); });
    const std::optional<uint32_t> graphics =
        graphicsPresent ? graphicsPresent : findFamily(families, vk::QueueFlagBits::eGraphics);
    const std::optional<uint32_t> present =
        graphicsPresent ? graphicsPresent
                        : findFamilyIf(families, [&](uint32_t family, vk::QueueFlags) { return supportsPresent(family); });
    if (!graphics || !present) {
        std::vector<std::string_view> missing;
        if (!graphics) {
            missing.emplace_back("graphics queue family");
        }
        if (!present) {
            missing.emplace_back("present queue family");
        }
        failStep("Queue family selection", missing);
    }
    graphicsIndex = *graphics;
    presentIndex = *present;

    // Dedicated (non-graphics) families let uploads and async compute overlap the frame.
    // Only a missing one falls back: shared non-graphics transfer+compute, any transfer+compute, graphics.
    const vk::QueueFlags transferCompute = vk::QueueFlagBits::eTransfer | vk::QueueFlagBits::eCompute;
    const uint32_t shared = findFamily(families, transferCompute, vk::QueueFlagBits::eGraphics)
                                .or_else([&] { return findFamily(families, transferCompute); })
                                .value_or(graphicsIndex);
    transferIndex = findFamily(families, vk::QueueFlagBits::eTransfer, vk::QueueFlagBits::eGraphics).value_or(shared);
    computeIndex = findFamily(families, vk::QueueFlagBits::eCompute, vk::QueueFlagBits::eGraphics).value_or(shared);
}

void Device::queryCapabilities()
{
    const std::vector<vk::ExtensionProperties> availableExtensions =
        physicalDevice.enumerateDeviceExtensionProperties();
    capabilities.hasClusterAccelerationStructure =
        isExtensionSupported(availableExtensions, vk::NVClusterAccelerationStructureExtensionName);
    capabilities.hasPartitionedAccelerationStructure =
        isExtensionSupported(availableExtensions, vk::NVPartitionedAccelerationStructureExtensionName);
    logInfo(std::format("Optional NV extensions: clusterAS={} partitionedAS={}",
                         capabilities.hasClusterAccelerationStructure,
                         capabilities.hasPartitionedAccelerationStructure),
             "Device");

    vk::StructureChain<
        vk::PhysicalDeviceProperties2, vk::PhysicalDeviceVulkan11Properties, vk::PhysicalDeviceVulkan12Properties,
        vk::PhysicalDeviceVulkan13Properties,
        vk::PhysicalDeviceVulkan14Properties, // after that comes things that may or may not be ext or promoted to khr
        vk::PhysicalDeviceBlendOperationAdvancedPropertiesEXT, vk::PhysicalDeviceDescriptorHeapPropertiesEXT,
        vk::PhysicalDeviceDescriptorIndexingPropertiesEXT, // Maybe not ext
        vk::PhysicalDeviceMeshShaderPropertiesEXT, vk::PhysicalDeviceDeviceGeneratedCommandsPropertiesEXT,
        vk::PhysicalDeviceMemoryDecompressionPropertiesEXT,
        vk::PhysicalDeviceHostImageCopyPropertiesEXT, // Maybe not ext
        vk::PhysicalDeviceTexelBufferAlignmentPropertiesEXT, // Maybe not ext + after that comes only khr when baseline
                                                             // 2060
        vk::PhysicalDeviceFragmentShadingRatePropertiesKHR,
        vk::PhysicalDeviceAccelerationStructurePropertiesKHR, vk::PhysicalDeviceOpacityMicromapPropertiesEXT,
        vk::PhysicalDeviceDepthStencilResolveProperties, vk::PhysicalDeviceDriverProperties,
        vk::PhysicalDeviceMaintenance3Properties, vk::PhysicalDeviceMaintenance4Properties,
        vk::PhysicalDeviceMaintenance5Properties, vk::PhysicalDeviceMaintenance6Properties,
        vk::PhysicalDeviceMaintenance7PropertiesKHR, vk::PhysicalDeviceMaintenance9PropertiesKHR,
        vk::PhysicalDeviceMaintenance10PropertiesKHR, vk::PhysicalDevicePipelineBinaryPropertiesKHR,
        vk::PhysicalDeviceRayTracingPipelinePropertiesKHR,
        vk::PhysicalDeviceRayTracingInvocationReorderPropertiesEXT,
        vk::PhysicalDevicePartitionedAccelerationStructurePropertiesNV,
        vk::PhysicalDeviceClusterAccelerationStructurePropertiesNV>
        propertiesChain;
    // structs of unsupported extensions must not be in the query chain
    if (!capabilities.hasPartitionedAccelerationStructure) {
        propertiesChain.unlink<vk::PhysicalDevicePartitionedAccelerationStructurePropertiesNV>();
    }
    if (!capabilities.hasClusterAccelerationStructure) {
        propertiesChain.unlink<vk::PhysicalDeviceClusterAccelerationStructurePropertiesNV>();
    }
    physicalDevice.getProperties2(&propertiesChain.get<vk::PhysicalDeviceProperties2>());
    capabilities.properties2 = propertiesChain.get<vk::PhysicalDeviceProperties2>();
    capabilities.vulkan11 = propertiesChain.get<vk::PhysicalDeviceVulkan11Properties>();
    capabilities.vulkan12 = propertiesChain.get<vk::PhysicalDeviceVulkan12Properties>();
    capabilities.vulkan13 = propertiesChain.get<vk::PhysicalDeviceVulkan13Properties>();
    capabilities.vulkan14 = propertiesChain.get<vk::PhysicalDeviceVulkan14Properties>();
    capabilities.blendOperationAdvanced = propertiesChain.get<vk::PhysicalDeviceBlendOperationAdvancedPropertiesEXT>();
    capabilities.descriptorHeap = propertiesChain.get<vk::PhysicalDeviceDescriptorHeapPropertiesEXT>();
    capabilities.descriptorIndexing = propertiesChain.get<vk::PhysicalDeviceDescriptorIndexingPropertiesEXT>();
    capabilities.meshShader = propertiesChain.get<vk::PhysicalDeviceMeshShaderPropertiesEXT>();
    capabilities.deviceGeneratedCommands =
        propertiesChain.get<vk::PhysicalDeviceDeviceGeneratedCommandsPropertiesEXT>();
    {
        const auto& dgc = capabilities.deviceGeneratedCommands;
        logInfo(std::format("DGC limits: maxSeq={} maxTokens={} maxTokenOffset={} maxStride={} "
                             "maxPipelines={} maxShaderObjects={} stages={:#x} pipelineBind={:#x} "
                             "shaderBind={:#x} xfb={} multiDrawCount={}",
                             dgc.maxIndirectSequenceCount, dgc.maxIndirectCommandsTokenCount,
                             dgc.maxIndirectCommandsTokenOffset, dgc.maxIndirectCommandsIndirectStride,
                             dgc.maxIndirectPipelineCount, dgc.maxIndirectShaderObjectCount,
                             static_cast<uint32_t>(dgc.supportedIndirectCommandsShaderStages),
                             static_cast<uint32_t>(dgc.supportedIndirectCommandsShaderStagesPipelineBinding),
                             static_cast<uint32_t>(dgc.supportedIndirectCommandsShaderStagesShaderBinding),
                             static_cast<bool>(dgc.deviceGeneratedCommandsTransformFeedback),
                             static_cast<bool>(dgc.deviceGeneratedCommandsMultiDrawIndirectCount)),
                 "Device");
    }
    capabilities.memoryDecompression = propertiesChain.get<vk::PhysicalDeviceMemoryDecompressionPropertiesEXT>();
    capabilities.hostImageCopy = propertiesChain.get<vk::PhysicalDeviceHostImageCopyPropertiesEXT>();
    {
        auto& hic = capabilities.hostImageCopy;
        capabilities.hostImageCopySrcLayouts.resize(hic.copySrcLayoutCount);
        capabilities.hostImageCopyDstLayouts.resize(hic.copyDstLayoutCount);

        vk::PhysicalDeviceHostImageCopyProperties layoutProps{
            .copySrcLayoutCount = hic.copySrcLayoutCount,
            .pCopySrcLayouts = capabilities.hostImageCopySrcLayouts.data(),
            .copyDstLayoutCount = hic.copyDstLayoutCount,
            .pCopyDstLayouts = capabilities.hostImageCopyDstLayouts.data(),
        };
        vk::PhysicalDeviceProperties2 props2{.pNext = &layoutProps};
        physicalDevice.getProperties2(&props2);

        hic.copySrcLayoutCount = layoutProps.copySrcLayoutCount;
        hic.copyDstLayoutCount = layoutProps.copyDstLayoutCount;
        hic.pCopySrcLayouts = capabilities.hostImageCopySrcLayouts.data();
        hic.pCopyDstLayouts = capabilities.hostImageCopyDstLayouts.data();

        const bool hasGeneralSrc =
            std::ranges::find(capabilities.hostImageCopySrcLayouts, vk::ImageLayout::eGeneral) !=
            capabilities.hostImageCopySrcLayouts.end();
        const bool hasGeneralDst =
            std::ranges::find(capabilities.hostImageCopyDstLayouts, vk::ImageLayout::eGeneral) !=
            capabilities.hostImageCopyDstLayouts.end();

        std::string uuidHex;
        uuidHex.reserve(VK_UUID_SIZE * 2);
        for (const uint8_t byte : hic.optimalTilingLayoutUUID) {
            uuidHex += std::format("{:02x}", byte);
        }
        logInfo(std::format("hostImageCopy: srcLayouts={} dstLayouts={} generalSrc={} generalDst={} "
                             "identicalMemTypes={} uuid={}",
                             hic.copySrcLayoutCount, hic.copyDstLayoutCount, hasGeneralSrc, hasGeneralDst,
                             static_cast<bool>(hic.identicalMemoryTypeRequirements), uuidHex),
                 "Device");
    }
    capabilities.texelBufferAlignment = propertiesChain.get<vk::PhysicalDeviceTexelBufferAlignmentPropertiesEXT>();
    capabilities.fragmentShadingRate = propertiesChain.get<vk::PhysicalDeviceFragmentShadingRatePropertiesKHR>();
    capabilities.accelerationStructure = propertiesChain.get<vk::PhysicalDeviceAccelerationStructurePropertiesKHR>();
    capabilities.depthStencilResolve = propertiesChain.get<vk::PhysicalDeviceDepthStencilResolveProperties>();
    capabilities.driverProperties = propertiesChain.get<vk::PhysicalDeviceDriverProperties>();
    capabilities.maintenance3 = propertiesChain.get<vk::PhysicalDeviceMaintenance3Properties>();
    capabilities.maintenance4 = propertiesChain.get<vk::PhysicalDeviceMaintenance4Properties>();
    capabilities.maintenance5 = propertiesChain.get<vk::PhysicalDeviceMaintenance5Properties>();
    {
        const auto& m5 = capabilities.maintenance5;
        logInfo(std::format("maintenance5: earlyCoverageAfterCount={} earlySampleMaskBeforeCount={} "
                             "depthStencilSwizzleOne={} polygonModePointSize={} "
                             "nonStrict1pxParallelogram={} nonStrictWideParallelogram={}",
                             static_cast<bool>(m5.earlyFragmentMultisampleCoverageAfterSampleCounting),
                             static_cast<bool>(m5.earlyFragmentSampleMaskTestBeforeSampleCounting),
                             static_cast<bool>(m5.depthStencilSwizzleOneSupport),
                             static_cast<bool>(m5.polygonModePointSize),
                             static_cast<bool>(m5.nonStrictSinglePixelWideLinesUseParallelogram),
                             static_cast<bool>(m5.nonStrictWideLinesUseParallelogram)),
                 "Device");
    }
    capabilities.maintenance6 = propertiesChain.get<vk::PhysicalDeviceMaintenance6Properties>();
    capabilities.maintenance7 = propertiesChain.get<vk::PhysicalDeviceMaintenance7PropertiesKHR>();
    capabilities.maintenance9 = propertiesChain.get<vk::PhysicalDeviceMaintenance9PropertiesKHR>();
    capabilities.maintenance10 = propertiesChain.get<vk::PhysicalDeviceMaintenance10PropertiesKHR>();
    capabilities.pipelineBinary = propertiesChain.get<vk::PhysicalDevicePipelineBinaryPropertiesKHR>();
    capabilities.rayTracingPipeline = propertiesChain.get<vk::PhysicalDeviceRayTracingPipelinePropertiesKHR>();
    capabilities.opacityMicromap = propertiesChain.get<vk::PhysicalDeviceOpacityMicromapPropertiesEXT>();
    capabilities.rayTracingInvocationReorder =
        propertiesChain.get<vk::PhysicalDeviceRayTracingInvocationReorderPropertiesEXT>();
    {
        // eNone: reorder calls are legal no-ops
        const auto& ser = capabilities.rayTracingInvocationReorder;
        logInfo(std::format("SER: hint={} maxSbtRecordIndex={}", vk::to_string(ser.rayTracingInvocationReorderReorderingHint),
                             ser.maxShaderBindingTableRecordIndex),
                 "Device");
    }
    capabilities.partitionedAccelerationStructure =
        propertiesChain.get<vk::PhysicalDevicePartitionedAccelerationStructurePropertiesNV>();
    capabilities.clusterAccelerationStructure =
        propertiesChain.get<vk::PhysicalDeviceClusterAccelerationStructurePropertiesNV>();
}

void Device::createLogicalDevice()
{
    const std::vector<QueueFamilyChain> queueFamilyChains =
        physicalDevice.getQueueFamilyProperties2<QueueFamilyChain>();
    logQueueFamilies(queueFamilyChains);
    std::vector<vk::QueueFamilyProperties> families;
    families.reserve(queueFamilyChains.size());
    for (const QueueFamilyChain& chain : queueFamilyChains) {
        families.push_back(chain.get<vk::QueueFamilyProperties2>().queueFamilyProperties);
    }
    findQueueFamilies(families);

    queryCapabilities();

    const std::vector<vk::ExtensionProperties> availableExtensions =
        physicalDevice.enumerateDeviceExtensionProperties();
    FeatureChain supportedFeatures;
    unlinkUnsupportedFeatures(supportedFeatures, availableExtensions);
    querySupportedFeatures(physicalDevice, supportedFeatures);
    const GiCacheSupport giCacheSupport = selectGiCache(availableExtensions, supportedFeatures);
    capabilities.giCache = giCacheSupport.cache;

    // only listed bits are requested; everything else stays false
    FeatureChain featureChain;
    unlinkUnsupportedFeatures(featureChain, availableExtensions);
    enableFeatures(featureChain, kRequiredFeatures);
    std::vector<const char*> enabledExtensions(kRequiredDeviceExtensions.begin(), kRequiredDeviceExtensions.end());
    if (capabilities.hasClusterAccelerationStructure) {
        enableFeatures(featureChain, kClusterAccelerationStructureFeatures);
        enabledExtensions.push_back(vk::NVClusterAccelerationStructureExtensionName);
    }
    if (capabilities.hasPartitionedAccelerationStructure) {
        enableFeatures(featureChain, kPartitionedAccelerationStructureFeatures);
        enabledExtensions.push_back(vk::NVPartitionedAccelerationStructureExtensionName);
    }
    // also under NRC, so both caches can run side by side (GI_CACHE_MODE both)
    if (giCacheSupport.hasSharc) {
        enableFeatures(featureChain, kSharcFeatures);
    }
    if (capabilities.giCache == GiCache::Nrc) {
        enableFeatures(featureChain, kNrcFeatures);
        enabledExtensions.insert(enabledExtensions.end(), kNrcDeviceExtensions.begin(), kNrcDeviceExtensions.end());
    }

    // one create info per distinct family; duplicates are a validation error.
    // the same set feeds concurrent sharing, where order does not matter
    queueFamilyIndices = {graphicsIndex, presentIndex, transferIndex, computeIndex};
    std::ranges::sort(queueFamilyIndices);
    const auto duplicates = std::ranges::unique(queueFamilyIndices);
    queueFamilyIndices.erase(duplicates.begin(), duplicates.end());

    constexpr float queuePriority = 0.0f;
    std::vector<vk::DeviceQueueCreateInfo> queueCreateInfos;
    queueCreateInfos.reserve(queueFamilyIndices.size());
    for (const uint32_t family : queueFamilyIndices) {
        queueCreateInfos.push_back({.queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &queuePriority});
    }

    vk::DeviceCreateInfo deviceCreateInfo{.pNext = &featureChain.get<vk::PhysicalDeviceFeatures2>(),
                                          .queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size()),
                                          .pQueueCreateInfos = queueCreateInfos.data(),
                                          .enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size()),
                                          .ppEnabledExtensionNames = enabledExtensions.data()};

    // requirements were checked above; this only catches driver-side failures
    try {
        vkdevice = vk::raii::Device(physicalDevice, deviceCreateInfo);
    } catch (const vk::SystemError& err) {
        const std::string message = std::format("Failed to create logical device: {}", err.what());
        logError(message, "Device");
        throw std::runtime_error(message);
    }

    setDebugName(vkdevice, instance, "Instance");
    setDebugName(vkdevice, physicalDevice, "PhysicalDevice");
    setDebugName(vkdevice, surface, "WindowSurface");
    setDebugName(vkdevice, vkdevice, "LogicalDevice");

    createQueues();
}

void Device::createQueues()
{
    graphicsQueue = vk::raii::Queue(vkdevice, graphicsIndex, 0);
    presentQueue = vk::raii::Queue(vkdevice, presentIndex, 0);
    transferQueue = vk::raii::Queue(vkdevice, transferIndex, 0);
    computeQueue = vk::raii::Queue(vkdevice, computeIndex, 0);

    setDebugName(vkdevice, graphicsQueue, "GraphicsQueue");
    setDebugName(vkdevice, presentQueue, "PresentQueue");
    setDebugName(vkdevice, transferQueue, "TransferQueue");
    setDebugName(vkdevice, computeQueue, "ComputeQueue");

    logInfo(std::format("Queue families: graphics={} present={} transfer={}{} compute={}{} (distinct {})",
                         graphicsIndex, presentIndex, transferIndex,
                         hasDedicatedTransferQueue() ? " (dedicated)" : " (shared)", computeIndex,
                         computeIndex != graphicsIndex ? " (async)" : " (shared)", queueFamilyIndices.size()),
             "Device");
}
