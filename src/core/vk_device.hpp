#pragma once
#include <span>
#include "types.hpp"

/**
 * @brief Manages the Vulkan device stack: instance, physical device, logical device, and queues.
 *
 * Device owns the RAII wrappers for core Vulkan objects. Stable handles and indices are public
 * for direct access after init(). Mutable/config values use accessors only when needed.
 *
 * Initialisation order (called via init()):
 *   1. createInstance()       – Vulkan instance + validation layers
 *   2. setupDebugMessenger()  – VK_EXT_debug_utils messenger (when validation is on)
 *   3. createSurface()        – SDL3 window surface
 *   4. pickPhysicalDevice()   – GPU selection by score
 *   5. createLogicalDevice()  – logical device + queues
 */
class Device
{
    void createInstance();
    void setupDebugMessenger(); // no-op when validation is off
    void createSurface();
    // rejects GPUs missing a required extension or feature; score: discrete +1000, plus maxImageDimension2D
    void pickPhysicalDevice();
    // prefers dedicated transfer/compute families, then a shared one, then graphics
    void findQueueFamilies(std::span<const vk::QueueFamilyProperties> families);
    // optional (NV) extensions are enabled only when supported, see HardwareCapabilities::has*
    void createLogicalDevice();
    void queryCapabilities();
    void createQueues();
    // highest count supported by both color and depth attachments
    [[nodiscard]] vk::SampleCountFlagBits getMaxUsableSampleCount() const;

public:
    /**
     * @brief Constructs a Device object.
     * @param window                SDL3 window used for surface creation; must outlive this object.
     * @param enableValidationLayers Enable Khronos validation layers and debug messenger output.
     */
    Device(SDL_Window* window, bool enableValidationLayers = false);

    /**
     * @brief Runs the full Vulkan device initialisation sequence.
     *
     * Must be called once before any other method.  Calls createInstance(),
     * setupDebugMessenger(), createSurface(), pickPhysicalDevice(), and
     * createLogicalDevice() in order.
     */
    void init();

    // -------------------------------------------------------------------------
    // Stable handles / indices — direct access (set during init, not mutated)
    // -------------------------------------------------------------------------

    SDL_Window* window = nullptr; ///< Non-owning pointer to the SDL3 window used for surface creation.
    bool enableValidationLayers = false; ///< Whether Khronos validation layers are active.

    vk::raii::Context context; ///< Top-level RAII context; loads the Vulkan loader at construction.
    vk::raii::Instance instance = nullptr; ///< Vulkan instance owning all per-application state.
    vk::raii::DebugUtilsMessengerEXT debugMessenger = nullptr; ///< Debug messenger; null when validation is disabled.
    vk::raii::PhysicalDevice physicalDevice = nullptr; ///< Selected GPU handle (highest-score candidate).
    vk::raii::SurfaceKHR surface = nullptr; ///< Platform surface created from the SDL3 window.
    vk::raii::Device vkdevice = nullptr; ///< Logical device; primary interface for GPU commands and resource creation.
    vk::raii::Queue graphicsQueue = nullptr; ///< Queue supporting graphics operations.
    vk::raii::Queue presentQueue = nullptr; ///< Queue supporting presentation; may alias graphicsQueue.
    vk::raii::Queue transferQueue = nullptr; ///< Transfer queue; aliases another family's queue when not dedicated.
    vk::raii::Queue computeQueue = nullptr; ///< Compute queue; aliases another family's queue when not dedicated.

    /// Unique queue family indices used by this device (passed to resource sharing mode setup).
    std::vector<uint32_t> queueFamilyIndices;

    /// Maximum MSAA sample count supported by both colour and depth attachments on this GPU.
    vk::SampleCountFlagBits msaaSamples = vk::SampleCountFlagBits::e1;

    uint32_t transferIndex = 0; ///< Queue family index of the transfer queue (always valid; may equal graphicsIndex).
    uint32_t computeIndex = 0; ///< Queue family index of the compute queue (always valid; may equal graphicsIndex).
    uint32_t graphicsIndex = 0; ///< Queue family index of the graphics queue.
    uint32_t presentIndex = 0; ///< Queue family index of the present queue.

    /// True when transfer work has its own queue family (separate pool / command buffers).
    [[nodiscard]] bool hasDedicatedTransferQueue() const noexcept
    {
        return transferIndex != graphicsIndex;
    }

    HardwareCapabilities capabilities =
        HardwareCapabilities{}; ///< Cached hardware capability support flags (e.g., ray-tracing, mesh shaders).
};
