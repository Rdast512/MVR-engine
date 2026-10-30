// Checks shaders/core/hiz.slang hizFootprint on spheres with hand-derived screen rects, depths and mips.
// The pyramid fetch (isHiZOccluded) needs the descriptor heap and is verified in captures instead.
#include "test_support.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>

using test::check;

namespace
{
    constexpr float kNear = 0.1f;
    constexpr float kFar = 100.0f;
    constexpr uint32_t kPyramidSize = 1024;
    constexpr float kTolerance = 1e-4f;

    // scalar layout: must match FootprintPush in shaders/hiz_footprint.slang
    struct FootprintPush
    {
        float viewProj[16]; // column-major
        VkDeviceAddress spheres;
        VkDeviceAddress results;
        uint32_t pyramidSize[2];
        uint32_t sphereCount;
    };
    static_assert(offsetof(FootprintPush, spheres) == 64);
    static_assert(offsetof(FootprintPush, pyramidSize) == 80);

    struct Sphere
    {
        float x, y, z, radius;
    };

    struct Expected
    {
        const char* name;
        Sphere sphere;
        bool isTestable;
        // only checked when testable
        std::array<float, 4> uvRect;
        float nearestViewZ; // view-space z of the nearest AABB face
        float lod;
    };

    // engine camera at the origin looking down -z: glm::perspectiveRH_ZO, 90 deg, aspect 1, y flipped
    void fillViewProj(float (&m)[16])
    {
        m[0] = 1.0f;
        m[5] = -1.0f;
        m[10] = kFar / (kNear - kFar);
        m[11] = -1.0f;
        m[14] = -(kFar * kNear) / (kFar - kNear);
    }

    float depthAt(float viewZ)
    {
        return (kFar / (kNear - kFar) * viewZ - (kFar * kNear) / (kFar - kNear)) / -viewZ;
    }

    float lodFor(float uvExtent)
    {
        return std::fmax(std::ceil(std::log2(uvExtent * static_cast<float>(kPyramidSize))) - 1.0f, 0.0f);
    }

    // centered unit sphere at distance 10: the nearest face (z = -9) bounds x, y to +-1/9 in ndc
    constexpr float kCentered = 0.5f / 9.0f;

    const std::array kCases = {
        Expected{"centered", {0.0f, 0.0f, -10.0f, 1.0f}, true,
                 {0.5f - kCentered, 0.5f - kCentered, 0.5f + kCentered, 0.5f + kCentered}, -9.0f, 6.0f},
        // x in [4, 6]: min at the far face (4 / 11), max at the near face (6 / 9)
        Expected{"offset", {5.0f, 0.0f, -10.0f, 1.0f}, true,
                 {0.5f + 2.0f / 11.0f, 0.5f - kCentered, 0.5f + 1.0f / 3.0f, 0.5f + kCentered}, -9.0f, 7.0f},
        Expected{"right edge, clamped", {12.0f, 0.0f, -10.0f, 1.0f}, true,
                 {1.0f, 0.5f - kCentered, 1.0f, 0.5f + kCentered}, -9.0f, 6.0f},
        Expected{"tiny and far", {0.0f, 0.0f, -50.0f, 0.001f}, true,
                 {0.5f - 0.0005f / 49.999f, 0.5f - 0.0005f / 49.999f, 0.5f + 0.0005f / 49.999f,
                  0.5f + 0.0005f / 49.999f},
                 -49.999f, 0.0f},
        Expected{"crosses near plane", {0.0f, 0.0f, -0.5f, 1.0f}, false, {}, 0.0f, 0.0f},
        Expected{"behind camera", {0.0f, 0.0f, 5.0f, 1.0f}, false, {}, 0.0f, 0.0f},
    };

    bool isNear(float actual, float expected) { return std::fabs(actual - expected) <= kTolerance; }

    void verify(const Expected& expected, const float* rect, const float* info)
    {
        const bool isTestable = info[2] != 0.0f;
        std::printf("  %s: testable %d rect (%.5f %.5f %.5f %.5f) depth %.6f lod %.0f\n", expected.name, isTestable,
                    rect[0], rect[1], rect[2], rect[3], info[0], info[1]);
        check(isTestable == expected.isTestable, expected.name);
        if (!expected.isTestable || !isTestable) {
            return;
        }
        bool isRectMatch = true;
        for (size_t i = 0; i < 4; ++i) {
            isRectMatch = isRectMatch && isNear(rect[i], expected.uvRect[i]);
        }
        check(isRectMatch, "  screen rect");
        check(std::fabs(info[0] - depthAt(expected.nearestViewZ)) <= 1e-5f, "  nearest depth");
        check(info[1] == expected.lod, "  mip");

        // finest mip where the rect spans at most 2 texels
        const float extent = std::fmax(rect[2] - rect[0], rect[3] - rect[1]) * static_cast<float>(kPyramidSize);
        const float lod = info[1];
        const bool isCovered = extent / std::exp2(lod) <= 2.0f;
        const bool isFinest = lod == 0.0f || extent / std::exp2(lod - 1.0f) > 2.0f;
        check(isCovered && isFinest, "  mip is the finest covering one");
        check(expected.lod == lodFor(std::fmax(expected.uvRect[2] - expected.uvRect[0],
                                               expected.uvRect[3] - expected.uvRect[1])),
              "  expected mip is self-consistent");
    }
}

int main()
{
    test::VulkanContext vulkan;
    const test::ContextStatus status = vulkan.create({}, {});
    if (status == test::ContextStatus::Unavailable) {
        return test::kSkipExitCode;
    }
    if (status != test::ContextStatus::Ready) {
        return test::finish();
    }

    const VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(FootprintPush)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    vkCreatePipelineLayout(vulkan.device, &layoutInfo, nullptr, &layout);

    const VkPipeline pipeline = test::createComputePipeline(
        vulkan, layout, std::string(HIZ_TEST_SHADER_DIR) + "/footprintMain.spv", "footprintMain");
    const bool hasPipeline = check(pipeline != VK_NULL_HANDLE, "footprint pipeline created");

    constexpr auto kCount = static_cast<uint32_t>(kCases.size());
    test::HostBuffer spheres = test::createHostBuffer(vulkan, kCount * sizeof(Sphere));
    test::HostBuffer results = test::createHostBuffer(vulkan, kCount * 2 * 4 * sizeof(float));

    if (hasPipeline) {
        auto* sphereData = static_cast<Sphere*>(spheres.mapped);
        for (uint32_t i = 0; i < kCount; ++i) {
            sphereData[i] = kCases[i].sphere;
        }
        FootprintPush push{};
        fillViewProj(push.viewProj);
        push.spheres = spheres.address;
        push.results = results.address;
        push.pyramidSize[0] = kPyramidSize;
        push.pyramidSize[1] = kPyramidSize;
        push.sphereCount = kCount;

        VkCommandBuffer cmd = vulkan.beginCommands();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, 1, 1, 1);
        VkMemoryBarrier hostBarrier{};
        hostBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        hostBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                             &hostBarrier, 0, nullptr, 0, nullptr);
        check(vulkan.submit(cmd), "footprint dispatch submitted");
        vulkan.waitIdle();

        const auto* rows = static_cast<const float*>(results.mapped);
        for (uint32_t i = 0; i < kCount; ++i) {
            verify(kCases[i], rows + i * 8, rows + i * 8 + 4);
        }
    }

    test::destroyHostBuffer(vulkan, spheres);
    test::destroyHostBuffer(vulkan, results);
    vkDestroyPipeline(vulkan.device, pipeline, nullptr);
    vkDestroyPipelineLayout(vulkan.device, layout, nullptr);
    return test::finish();
}
