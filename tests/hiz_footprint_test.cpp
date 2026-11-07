// Checks shaders/core/culling.slang on spheres with hand-derived screen rects, depths, mips and sub-pixel results,
// that the chosen mip lets one center fetch cover the rect, and that the box path (intersectFrustum on the sphere's
// world AABB) agrees with the sphere path (intersectFrustumSphere). Every case runs in both depth conventions.
// The pyramid fetch (isHiZOccluded) needs the descriptor heap and is verified in captures instead.
#include "test_support.hpp"

#include <algorithm>
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
    // subPixelPrecisionBits = 8
    constexpr float kSubpixelSnap = 1.0f / 512.0f;
    // per sphere: sphere path (uvRect, info), then box path (uvRect, info)
    constexpr uint32_t kResultRows = 4;

    // scalar layout: must match FootprintPush in shaders/hiz_footprint.slang
    struct FootprintPush
    {
        float viewProj[16]; // column-major
        VkDeviceAddress spheres;
        VkDeviceAddress results;
        uint32_t pyramidSize[2];
        uint32_t sphereCount;
        float subpixelSnap;
    };
    static_assert(offsetof(FootprintPush, spheres) == 64);
    static_assert(offsetof(FootprintPush, pyramidSize) == 80);
    static_assert(offsetof(FootprintPush, subpixelSnap) == 92);

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

    // the module and projection under test; reversed swaps near and far as Camera::updateProjection does
    struct DepthConvention
    {
        const char* name;
        const char* shaderFile;
        bool isReversed;
    };
    constexpr std::array kConventions = {
        DepthConvention{"standard Z", "footprintStandard.spv", false},
        DepthConvention{"reverse Z", "footprintReversed.spv", true},
    };

    // glm::perspectiveRH_ZO(90 deg, 1, zNear, zFar) with y flipped; reverse Z passes (far, near)
    float depthScale(bool isReversed) { return isReversed ? kNear / (kFar - kNear) : kFar / (kNear - kFar); }
    float depthOffset(bool isReversed)
    {
        return isReversed ? kFar * kNear / (kFar - kNear) : -(kFar * kNear) / (kFar - kNear);
    }

    // engine camera at the origin looking down -z
    void fillViewProj(float (&m)[16], bool isReversed)
    {
        m[0] = 1.0f;
        m[5] = -1.0f;
        m[10] = depthScale(isReversed);
        m[11] = -1.0f;
        m[14] = depthOffset(isReversed);
    }

    float depthAt(float viewZ, bool isReversed)
    {
        return (depthScale(isReversed) * viewZ + depthOffset(isReversed)) / -viewZ;
    }

    // mirrors isSubPixel with the pyramid as the viewport: no pixel center k + 0.5 inside the widened rect on one axis
    bool isSubPixelFor(const std::array<float, 4>& rect)
    {
        const auto size = static_cast<float>(kPyramidSize);
        const auto isEmpty = [size](float lo, float hi) {
            return std::floor(hi * size + kSubpixelSnap - 0.5f) < std::ceil(lo * size - kSubpixelSnap - 0.5f);
        };
        return isEmpty(rect[0], rect[2]) || isEmpty(rect[1], rect[3]);
    }

    // mirrors hizMip: the 1-texel mip, or one finer when the rect fits a 2x2 block there without straddling
    float lodFor(const std::array<float, 4>& rect)
    {
        const auto pyramid = static_cast<float>(kPyramidSize);
        float lod = std::fmax(std::ceil(std::log2(std::fmax(rect[2] - rect[0], rect[3] - rect[1]) * pyramid)), 0.0f);
        if (lod > 0.0f) {
            const float fine = std::fmax(pyramid * std::exp2(1.0f - lod), 1.0f);
            const auto fits = [fine](float lo, float hi) {
                return (lo * fine - std::floor(lo * fine)) + (hi - lo) * fine <= 2.0f;
            };
            if (fits(rect[0], rect[2]) && fits(rect[1], rect[3])) {
                lod -= 1.0f;
            }
        }
        return lod;
    }

    // whether the bilinear 2x2 footprint of one clamp-to-edge fetch at the center of [lo, hi] covers its texels
    bool isCoveredByCenterFetch(float lo, float hi, float lod)
    {
        const float size = std::fmax(std::floor(static_cast<float>(kPyramidSize) / std::exp2(lod)), 1.0f);
        const auto texel = [size](float t) { return std::clamp(t, 0.0f, size - 1.0f); };
        const float first = texel(std::floor(lo * size));
        const float last = std::fmax(texel(std::ceil(hi * size) - 1.0f), first);
        const float footprint = std::floor((lo + hi) * 0.5f * size - 0.5f);
        return first >= texel(footprint) && last <= texel(footprint + 1.0f);
    }

    bool isCoveredAt(const float* rect, float lod)
    {
        return isCoveredByCenterFetch(rect[0], rect[2], lod) && isCoveredByCenterFetch(rect[1], rect[3], lod);
    }

    // centered unit sphere at distance 10: the nearest face (z = -9) bounds x, y to +-1/9 in ndc
    constexpr float kCentered = 0.5f / 9.0f;

    const std::array kCases = {
        Expected{"centered", {0.0f, 0.0f, -10.0f, 1.0f}, true,
                 {0.5f - kCentered, 0.5f - kCentered, 0.5f + kCentered, 0.5f + kCentered}, -9.0f, 6.0f},
        // x in [4, 6]: min at the far face (4 / 11), max at the near face (6 / 9)
        Expected{"offset", {5.0f, 0.0f, -10.0f, 1.0f}, true,
                 {0.5f + 2.0f / 11.0f, 0.5f - kCentered, 0.5f + 1.0f / 3.0f, 0.5f + kCentered}, -9.0f, 7.0f},
        // x in [-0.113, 1.887], both ends at the near face. One mip finer the rect would start at 0.9 of a texel and
        // span 1.78 more, straddling three texels, so the 1-texel mip stays
        Expected{"straddles, no refinement", {0.887f, 0.0f, -10.0f, 1.0f}, true,
                 {0.5f - 0.113f / 18.0f, 0.5f - kCentered, 0.5f + 1.887f / 18.0f, 0.5f + kCentered}, -9.0f, 7.0f},
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

    // the box path must reproduce the sphere path: same corners, same accumulation
    void verifyBoxPath(const float* sphereRow, const float* boxRow)
    {
        const bool isValid = sphereRow[6] != 0.0f;
        bool isMatch = boxRow[6] == sphereRow[6] && boxRow[7] == sphereRow[7];
        if (isValid) {
            for (size_t i = 0; i < 4; ++i) {
                isMatch = isMatch && isNear(boxRow[i], sphereRow[i]);
            }
            isMatch = isMatch && std::fabs(boxRow[4] - sphereRow[4]) <= 1e-6f && boxRow[5] == sphereRow[5];
        }
        check(isMatch, "  box path matches the sphere path");
    }

    void verify(const Expected& expected, const float* row, bool isReversed)
    {
        const float* rect = row;
        const float* info = row + 4;
        const bool isTestable = info[2] != 0.0f;
        const bool isSubPixel = info[3] != 0.0f;
        std::printf("  %s: testable %d rect (%.5f %.5f %.5f %.5f) depth %.6f lod %.0f sub-pixel %d\n", expected.name,
                    isTestable, rect[0], rect[1], rect[2], rect[3], info[0], info[1], isSubPixel);
        check(isTestable == expected.isTestable, expected.name);
        verifyBoxPath(row, row + 8);
        if (!expected.isTestable || !isTestable) {
            check(!isSubPixel, "  never sub-pixel when not testable");
            return;
        }
        check(isSubPixel == isSubPixelFor(expected.uvRect), "  sub-pixel");
        bool isRectMatch = true;
        for (size_t i = 0; i < 4; ++i) {
            isRectMatch = isRectMatch && isNear(rect[i], expected.uvRect[i]);
        }
        check(isRectMatch, "  screen rect");
        check(std::fabs(info[0] - depthAt(expected.nearestViewZ, isReversed)) <= 1e-5f, "  nearest depth");
        check(info[1] == expected.lod, "  mip");

        // isHiZOccluded fetches once at the rect center: that fetch must cover the rect, one mip finer must not
        const float lod = info[1];
        check(isCoveredAt(rect, lod), "  center fetch covers the rect");
        check(lod == 0.0f || !isCoveredAt(rect, lod - 1.0f), "  mip is the finest covering one");
        check(expected.lod == lodFor(expected.uvRect), "  expected mip is self-consistent");
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

    constexpr auto kCount = static_cast<uint32_t>(kCases.size());
    test::HostBuffer spheres = test::createHostBuffer(vulkan, kCount * sizeof(Sphere));
    test::HostBuffer results = test::createHostBuffer(vulkan, kCount * kResultRows * 4 * sizeof(float));

    for (const DepthConvention& convention : kConventions) {
        std::printf("%s\n", convention.name);
        const VkPipeline pipeline = test::createComputePipeline(
            vulkan, layout, std::string(HIZ_TEST_SHADER_DIR) + "/" + convention.shaderFile, "footprintMain");
        if (!check(pipeline != VK_NULL_HANDLE, "footprint pipeline created")) {
            continue;
        }
        auto* sphereData = static_cast<Sphere*>(spheres.mapped);
        for (uint32_t i = 0; i < kCount; ++i) {
            sphereData[i] = kCases[i].sphere;
        }
        FootprintPush push{};
        fillViewProj(push.viewProj, convention.isReversed);
        push.spheres = spheres.address;
        push.results = results.address;
        push.pyramidSize[0] = kPyramidSize;
        push.pyramidSize[1] = kPyramidSize;
        push.sphereCount = kCount;
        push.subpixelSnap = kSubpixelSnap;

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
            verify(kCases[i], rows + i * kResultRows * 4, convention.isReversed);
        }
        vkDestroyPipeline(vulkan.device, pipeline, nullptr);
    }

    test::destroyHostBuffer(vulkan, spheres);
    test::destroyHostBuffer(vulkan, results);
    vkDestroyPipelineLayout(vulkan.device, layout, nullptr);
    return test::finish();
}
