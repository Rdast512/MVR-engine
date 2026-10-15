#pragma once
#include <vulkan/vulkan_raii.hpp>
#include "vk_device.hpp"
#include <vulkan/vulkan_enums.hpp>
// TODO add callback for window resize

class SwapChain
{
    static vk::SurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<vk::SurfaceFormatKHR> &availableFormats);
    static vk::PresentModeKHR chooseSwapPresentMode(const std::vector<vk::PresentModeKHR> &availablePresentModes);
    vk::Extent2D chooseSwapExtent(const vk::SurfaceCapabilitiesKHR &capabilities) const;
    void createSwapChain();
    void createImageViews();
public:
    SwapChain(SDL_Window *window, const Device &device);
    ~SwapChain() = default;

    void init();

    SDL_Window *window = nullptr;
    const Device &device;

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

    void recreateSwapChain();
    void cleanupSwapChain();
};