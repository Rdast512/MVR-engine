#pragma once
#include <vulkan/vulkan_raii.hpp>
#include "vk_device.hpp"
#include <vulkan/vulkan_enums.hpp>

class SwapChain
{
    static vk::SurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<vk::SurfaceFormat2KHR> &availableFormats);
    [[nodiscard]] vk::PresentModeKHR chooseSwapPresentMode(
        const std::vector<vk::PresentModeKHR> &availablePresentModes) const;
    vk::Extent2D chooseSwapExtent(const vk::SurfaceCapabilitiesKHR &capabilities) const;
    // oldSwapChain: retired by the new one, which may reuse its resources
    void createSwapChain(vk::SwapchainKHR oldSwapChain = nullptr);
    void createImageViews();
public:
    SwapChain(SDL_Window *window, const Device &device);
    ~SwapChain() = default;

    void init();

    SDL_Window *window = nullptr;
    const Device &device;
    // set before init: IMMEDIATE when available, so GPU clocks are not idled by vsync (bench runs)
    bool isUncapped = false;

    const vk::raii::PhysicalDevice &physicalDevice;
    const vk::raii::SurfaceKHR &surface;
    const vk::raii::Device &vkDevice;
    const std::vector<uint32_t> &queueFamilyIndices;

    vk::raii::SwapchainKHR swapChain = nullptr;
    vk::SurfaceFormatKHR swapChainSurfaceFormat;
    vk::Extent2D swapChainExtent{};
    std::vector<vk::Image> swapChainImages;
    vk::Format swapChainImageFormat = vk::Format::eUndefined;
    std::vector<vk::raii::ImageView> swapChainImageViews;

    // device idle: replaces the swapchain at the surface's current extent
    void recreateSwapChain();
};