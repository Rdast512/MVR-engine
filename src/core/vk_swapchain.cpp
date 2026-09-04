#include "vk_swapchain.hpp"

#include "../util/debug.hpp"
#include "../static_headers/logger.hpp"
#include "../util/vk_tracy.hpp"

// TODO use swapchain maintenance1 special fence so i can remove other present fences

SwapChain::SwapChain(SDL_Window *window, const Device &device)
    : window(window),
      device(device),
      physicalDevice(device.physicalDevice),
      surface(device.surface),
      vkDevice(device.vkdevice),
      queueFamilyIndices(device.queueFamilyIndices) {}

void SwapChain::init() {
    ZoneScopedN("SwapChain::init");
    log_info("Initialized SwapChain", "SwapChain");
    createSwapChain();
    createImageViews();
}

vk::SurfaceFormatKHR SwapChain::chooseSwapSurfaceFormat(
    const std::vector<vk::SurfaceFormatKHR> &availableFormats) {
    for (const auto &availableFormat : availableFormats) {
        if (availableFormat.format == vk::Format::eB8G8R8A8Srgb &&
            availableFormat.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
            return availableFormat;
        }
    }
    return availableFormats[0];
}

vk::PresentModeKHR SwapChain::chooseSwapPresentMode(
    const std::vector<vk::PresentModeKHR> &availablePresentModes) {
    for (const auto &availablePresentMode : availablePresentModes) {
        if (availablePresentMode == vk::PresentModeKHR::eFifoLatestReady) {
            return availablePresentMode;
        }
    }
    return vk::PresentModeKHR::eFifo;
}

vk::Extent2D SwapChain::chooseSwapExtent(
    const vk::SurfaceCapabilitiesKHR &capabilities) const {
    if (capabilities.currentExtent.width !=
        std::numeric_limits<uint32_t>::max()) {
        return capabilities.currentExtent;
    }
    int width, height;
    SDL_GetWindowSizeInPixels(window, &width, &height);
    log_info(std::format("Window size: {}x{}", width, height), "SwapChain");

    return {std::clamp<uint32_t>(static_cast<uint32_t>(width), capabilities.minImageExtent.width,
                                 capabilities.maxImageExtent.width),
            std::clamp<uint32_t>(static_cast<uint32_t>(height), capabilities.minImageExtent.height,
                                 capabilities.maxImageExtent.height)};
}


void SwapChain::createSwapChain() {
    ZoneScopedN("SwapChain::createSwapChain");

    // Prefer surface_capabilities2 (avoids WARNING-legacy-gpdsc2 / getSurfaceCapabilitiesKHR).
    const vk::PhysicalDeviceSurfaceInfo2KHR surfaceInfo{.surface = *surface};
    const vk::SurfaceCapabilitiesKHR surfaceCapabilities =
        physicalDevice.getSurfaceCapabilities2KHR(surfaceInfo).surfaceCapabilities;
    swapChainSurfaceFormat =
        chooseSwapSurfaceFormat(physicalDevice.getSurfaceFormatsKHR(surface));
    swapChainImageFormat = swapChainSurfaceFormat.format;
    swapChainExtent = chooseSwapExtent(surfaceCapabilities);
    auto minImageCount = std::max(3u, surfaceCapabilities.minImageCount);

    minImageCount = (surfaceCapabilities.maxImageCount > 0 &&
                     minImageCount > surfaceCapabilities.maxImageCount)
                        ? surfaceCapabilities.maxImageCount
                        : minImageCount;
    // Choose the present mode first; the compatible-modes query below depends on it.
    const vk::PresentModeKHR chosenPresentMode =
        chooseSwapPresentMode(physicalDevice.getSurfacePresentModesKHR(*surface));

    // VK_KHR_swapchain_maintenance1 validation requires that the present modes listed in
    // VkSwapchainPresentModesCreateInfoKHR::pPresentModes are a subset of those returned by
    // VkSurfacePresentModeCompatibilityKHR for the *chosen* present mode – passing the full
    // set from vkGetPhysicalDeviceSurfacePresentModesKHR is incorrect and triggers a
    // validation error.  We query the compatible modes via the two-step
    // vkGetPhysicalDeviceSurfaceCapabilities2KHR pattern, injecting the chosen mode through
    // a VkSurfacePresentModeKHR pNext chain on VkPhysicalDeviceSurfaceInfo2KHR.
    vk::SurfacePresentModeKHR surfacePresentModeKHR{.presentMode = chosenPresentMode};
    vk::PhysicalDeviceSurfaceInfo2KHR surfaceInfo2{.surface = *surface};
    surfaceInfo2.pNext = &surfacePresentModeKHR;

    // Step 1: get the compatible mode count via the high-level template API.
    // The StructureChain correctly wires SurfaceCapabilities2KHR→SurfacePresentModeCompatibilityKHR;
    // the driver fills in presentModeCount while pPresentModes remains null.
    auto caps2Chain = physicalDevice.getSurfaceCapabilities2KHR<
        vk::SurfaceCapabilities2KHR, vk::SurfacePresentModeCompatibilityKHR>(surfaceInfo2);

    // Step 2: allocate the array, inject the pointer into the already-linked chain, and re-query.
    // getSurfaceCapabilities2KHR always allocates a fresh StructureChain internally and has no
    // overload that accepts a pre-allocated output array, so the raw dispatcher call is required
    // for this fill step only.
    std::vector<vk::PresentModeKHR> compatibleModes(
        caps2Chain.get<vk::SurfacePresentModeCompatibilityKHR>().presentModeCount);
    caps2Chain.get<vk::SurfacePresentModeCompatibilityKHR>().pPresentModes = compatibleModes.data();
    physicalDevice.getDispatcher()->vkGetPhysicalDeviceSurfaceCapabilities2KHR(
        *physicalDevice,
        reinterpret_cast<const VkPhysicalDeviceSurfaceInfo2KHR*>(&surfaceInfo2),
        reinterpret_cast<VkSurfaceCapabilities2KHR*>(&caps2Chain.get<vk::SurfaceCapabilities2KHR>()));

    log_info(std::format("Chosen present mode: {} | compatible modes:", vk::to_string(chosenPresentMode)), "SwapChain");
    for (const auto& mode : compatibleModes) {
        log_info(std::format("  {}", vk::to_string(mode)), "SwapChain");
    }

    vk::SwapchainPresentModesCreateInfoKHR modesInfo{
        .presentModeCount = static_cast<uint32_t>(compatibleModes.size()),
        .pPresentModes = compatibleModes.data()
    };

    vk::SwapchainCreateInfoKHR swapChainCreateInfo{
        .pNext = &modesInfo,
        .flags = vk::SwapchainCreateFlagsKHR(),
        .surface = *surface,
        .minImageCount = minImageCount,
        .imageFormat = swapChainSurfaceFormat.format,
        .imageColorSpace = swapChainSurfaceFormat.colorSpace,
        .imageExtent = swapChainExtent,
        .imageArrayLayers = 1,
        .imageUsage = vk::ImageUsageFlagBits::eColorAttachment,
        .imageSharingMode = queueFamilyIndices.size() > 1
                                ? vk::SharingMode::eConcurrent
                                : vk::SharingMode::eExclusive,
        .queueFamilyIndexCount =
            static_cast<uint32_t>(queueFamilyIndices.size()),
        .pQueueFamilyIndices = queueFamilyIndices.data(),
        .preTransform = surfaceCapabilities.currentTransform,
        .compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque,
        .presentMode = chosenPresentMode,
        .clipped = true,
        .oldSwapchain = nullptr};

    swapChain = vk::raii::SwapchainKHR(vkDevice, swapChainCreateInfo);
    swapChainImages = swapChain.getImages();
    for (size_t i = 0; i < swapChainImages.size(); ++i) {
        setDebugName(vkDevice, swapChainImages[i], std::format("SwapchainImage_{}", i));
    }
 }


 void SwapChain::createImageViews() {
     ZoneScopedN("SwapChain::createImageViews");
     vk::ImageViewCreateInfo imageViewCreateInfo{
         .viewType = vk::ImageViewType::e2D,
         .format = swapChainImageFormat,
         .subresourceRange = {vk::ImageAspectFlagBits::eColor,  0, 1, 0, 1}};
     for (auto image : swapChainImages) {
         imageViewCreateInfo.image = image;
         swapChainImageViews.emplace_back(vkDevice, imageViewCreateInfo);
         setDebugName(vkDevice, swapChainImageViews.back(), std::format("SwapchainImageView_{}", swapChainImageViews.size() - 1));
     }
 }

void SwapChain::cleanupSwapChain() {
    ZoneScopedN("SwapChain::cleanupSwapChain");
    swapChainImageViews.clear();
    swapChain = nullptr;
}

void SwapChain::recreateSwapChain() {
    ZoneScopedN("SwapChain::recreateSwapChain");
    vkDevice.waitIdle();

    cleanupSwapChain();

    createSwapChain();
    createImageViews();
}