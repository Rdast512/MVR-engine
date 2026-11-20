// Checks RTXDI bridge GI math and guards directly on the GPU: LogLuv GI reservoir packing across 8 orders of
// magnitude, RAB_ValidateGISampleWithJacobian bounds against NVIDIA RAB_SpatialHelpers.hlsli, shadeGiSample /
// RAB_GetGISampleTargetPdfForSurface against an independent double-precision glTF BRDF reference, and CheckedBuffer
// out-of-bounds guards (considerations C-F3-1).
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

using test::check;

namespace
{
    constexpr double kPi = 3.14159265358979323846;

    // scalar layout: must match RtxdiMathPush in tests/shaders/rtxdi_math.slang
    struct RtxdiMathPush
    {
        VkDeviceAddress radianceInputs;
        VkDeviceAddress radianceOutputs;
        VkDeviceAddress jacobianInputs;
        VkDeviceAddress jacobianOutputs;
        VkDeviceAddress shadeOutputs;
        VkDeviceAddress guardedBuffer;
        uint32_t radianceCount;
        uint32_t jacobianCount;
    };
    static_assert(sizeof(RtxdiMathPush) == 56);

    struct Vec4
    {
        float x, y, z, w;
    };

    struct Vec3d
    {
        double x, y, z;
    };

    double dot(Vec3d a, Vec3d b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    Vec3d normalize(Vec3d v)
    {
        const double len = std::sqrt(dot(v, v));
        return {v.x / len, v.y / len, v.z / len};
    }

    // independent double-precision reference for shadeGiSample
    Vec3d referenceShadeGiSample()
    {
        const Vec3d n{0.0, 0.0, 1.0};
        const Vec3d v = normalize({0.5, 0.0, 0.8660254});
        const Vec3d l = normalize({-1.0, 0.5, 1.5});
        const Vec3d cDiff{0.7, 0.25, 0.1};
        const Vec3d f0{0.04, 0.04, 0.04};
        const Vec3d rad{2.0, 1.5, 0.5};
        const double alpha = 0.4 * 0.4;
        const double a2 = alpha * alpha;
        const double nDotL = dot(n, l);
        const double nDotV = dot(n, v);
        const Vec3d h = normalize({l.x + v.x, l.y + v.y, l.z + v.z});
        const double nDotH = std::clamp(dot(n, h), 0.0, 1.0);
        const double vDotH = std::clamp(dot(v, h), 0.0, 1.0);
        const double dDenom = nDotH * nDotH * (a2 - 1.0) + 1.0;
        const double D = a2 / (kPi * dDenom * dDenom);
        const double ggxV = nDotL * std::sqrt(nDotV * nDotV * (1.0 - a2) + a2);
        const double ggxL = nDotV * std::sqrt(nDotL * nDotL * (1.0 - a2) + a2);
        const double V = 0.5 / std::max(ggxV + ggxL, 1e-7);
        const double pow5 = std::pow(1.0 - vDotH, 5.0);
        const double fx = f0.x + (1.0 - f0.x) * pow5;
        const double fy = f0.y + (1.0 - f0.y) * pow5;
        const double fz = f0.z + (1.0 - f0.z) * pow5;
        return {
            ((1.0 - fx) * cDiff.x / kPi + fx * D * V) * rad.x * nDotL,
            ((1.0 - fy) * cDiff.y / kPi + fy * D * V) * rad.y * nDotL,
            ((1.0 - fz) * cDiff.z / kPi + fz * D * V) * rad.z * nDotL,
        };
    }

    bool relClose(float actual, float expected, float tol)
    {
        if (expected == 0.0f) {
            return std::fabs(actual) <= tol;
        }
        return std::fabs(actual - expected) <= tol * std::fmax(std::fabs(expected), 1e-6f);
    }
} // namespace

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

    const VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(RtxdiMathPush)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    vkCreatePipelineLayout(vulkan.device, &layoutInfo, nullptr, &layout);

    const VkPipeline pipeline = test::createComputePipeline(
        vulkan, layout, std::string(RTXDI_MATH_TEST_SHADER_DIR) + "/rtxdiMathMain.spv", "rtxdiMathMain");
    if (check(pipeline != VK_NULL_HANDLE, "rtxdi_math pipeline created")) {
        // LogLuv round-trip across dim indirect gallery radiance (1e-4) up to bright sunlit bounces (1e3)
        const std::vector<Vec4> radianceInputs{
            {0.0f, 0.0f, 0.0f, 0.0f},
            {0.0002f, 0.00018f, 0.00015f, 3.14f},
            {0.05f, 0.02f, 0.01f, 1.25f},
            {0.8f, 0.4f, 0.2f, 0.5f},
            {12.5f, 8.0f, 3.2f, 2.0f},
            {850.0f, 600.0f, 400.0f, 0.1f},
        };
        struct JacobianCase
        {
            float input;
            bool expectedValid;
            float expectedClamped;
        };
        const std::vector<JacobianCase> jacobianCases{
            {0.0f, false, 0.0f},
            {0.099f, false, 0.099f},
            {0.101f, true, 1.0f / 3.0f},
            {0.25f, true, 1.0f / 3.0f},
            {0.5f, true, 0.5f},
            {1.0f, true, 1.0f},
            {2.5f, true, 2.5f},
            {5.0f, true, 3.0f},
            {9.99f, true, 3.0f},
            {10.01f, false, 10.01f},
        };

        test::HostBuffer radIn = test::createHostBuffer(vulkan, radianceInputs.size() * sizeof(Vec4));
        test::HostBuffer radOut = test::createHostBuffer(vulkan, radianceInputs.size() * sizeof(Vec4));
        test::HostBuffer jacIn = test::createHostBuffer(vulkan, jacobianCases.size() * sizeof(float));
        test::HostBuffer jacOut = test::createHostBuffer(vulkan, jacobianCases.size() * sizeof(Vec4));
        test::HostBuffer shadeOut = test::createHostBuffer(vulkan, 4 * sizeof(Vec4));
        test::HostBuffer guarded = test::createHostBuffer(vulkan, 4 * sizeof(uint32_t));

        auto* radInPtr = static_cast<Vec4*>(radIn.mapped);
        for (size_t i = 0; i < radianceInputs.size(); ++i) {
            radInPtr[i] = radianceInputs[i];
        }
        auto* jacInPtr = static_cast<float*>(jacIn.mapped);
        for (size_t i = 0; i < jacobianCases.size(); ++i) {
            jacInPtr[i] = jacobianCases[i].input;
        }
        auto* guardedPtr = static_cast<uint32_t*>(guarded.mapped);
        guardedPtr[3] = 42u;

        const RtxdiMathPush push{
            .radianceInputs = radIn.address,
            .radianceOutputs = radOut.address,
            .jacobianInputs = jacIn.address,
            .jacobianOutputs = jacOut.address,
            .shadeOutputs = shadeOut.address,
            .guardedBuffer = guarded.address,
            .radianceCount = static_cast<uint32_t>(radianceInputs.size()),
            .jacobianCount = static_cast<uint32_t>(jacobianCases.size()),
        };

        VkCommandBuffer cmd = vulkan.beginCommands();
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, 1, 1, 1);
        VkMemoryBarrier hostBarrier{};
        hostBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        hostBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hostBarrier,
                             0, nullptr, 0, nullptr);
        check(vulkan.submit(cmd), "rtxdi_math dispatch submitted");
        vulkan.waitIdle();

        // 1. LogLuv packing round-trip (32-bit LogLuv has ~2-3% chromaticity/luma quantization error)
        const auto* radOutPtr = static_cast<const Vec4*>(radOut.mapped);
        for (size_t i = 0; i < radianceInputs.size(); ++i) {
            const Vec4& in = radianceInputs[i];
            const Vec4& out = radOutPtr[i];
            const bool isZero = in.x == 0.0f && in.y == 0.0f && in.z == 0.0f;
            const bool ok = isZero
                ? (out.x == 0.0f && out.y == 0.0f && out.z == 0.0f && out.w == 0.0f)
                : (relClose(out.x, in.x, 0.04f) && relClose(out.y, in.y, 0.04f) && relClose(out.z, in.z, 0.04f) &&
                   relClose(out.w, in.w, 1e-5f));
            std::printf("  LogLuv[%zu]: in (%.5g %.5g %.5g) -> out (%.5g %.5g %.5g)\n", i, in.x, in.y, in.z, out.x,
                        out.y, out.z);
            check(ok, "  LogLuv GI reservoir round-trip within 4%");
        }

        // 2. Jacobian validation bounds
        const auto* jacOutPtr = static_cast<const Vec4*>(jacOut.mapped);
        for (size_t i = 0; i < jacobianCases.size(); ++i) {
            const bool valid = jacOutPtr[i].x > 0.5f;
            const bool ok = valid == jacobianCases[i].expectedValid &&
                (!valid || relClose(jacOutPtr[i].y, jacobianCases[i].expectedClamped, 1e-5f));
            check(ok, "  RAB_ValidateGISampleWithJacobian matches NVIDIA bounds");
        }

        // 3. shadeGiSample and target PDF
        const auto* shadePtr = static_cast<const Vec4*>(shadeOut.mapped);
        const Vec3d refShade = referenceShadeGiSample();
        const float refLum = static_cast<float>(0.2126 * refShade.x + 0.7152 * refShade.y + 0.0722 * refShade.z);
        std::printf("  shadeGiSample: actual (%.5f %.5f %.5f) pdf %.5f vs ref (%.5f %.5f %.5f) pdf %.5f\n",
                    shadePtr[0].x, shadePtr[0].y, shadePtr[0].z, shadePtr[0].w, refShade.x, refShade.y, refShade.z,
                    refLum);
        check(relClose(shadePtr[0].x, static_cast<float>(refShade.x), 2e-4f) &&
                  relClose(shadePtr[0].y, static_cast<float>(refShade.y), 2e-4f) &&
                  relClose(shadePtr[0].z, static_cast<float>(refShade.z), 2e-4f),
              "  shadeGiSample matches double-precision glTF BRDF reference");
        check(relClose(shadePtr[0].w, refLum, 2e-4f), "  RAB_GetGISampleTargetPdfForSurface equals luminance");
        check(shadePtr[1].x == 0.0f && shadePtr[1].y == 0.0f && shadePtr[1].z == 0.0f && shadePtr[1].w == 0.0f,
              "  shadeGiSample returns 0 below hemisphere");
        check(shadePtr[2].x == 0.0f && shadePtr[2].y == 0.0f && shadePtr[2].z == 0.0f && shadePtr[2].w == 0.0f,
              "  shadeGiSample returns 0 for unlit surface");

        // 4. CheckedBuffer bounds guard
        check(shadePtr[3].x > 0.5f && shadePtr[3].y > 0.5f && shadePtr[3].z > 0.5f && shadePtr[3].w > 0.5f,
              "  CheckedBuffer in-bounds access works and OOB reads return 0");

        test::destroyHostBuffer(vulkan, radIn);
        test::destroyHostBuffer(vulkan, radOut);
        test::destroyHostBuffer(vulkan, jacIn);
        test::destroyHostBuffer(vulkan, jacOut);
        test::destroyHostBuffer(vulkan, shadeOut);
        test::destroyHostBuffer(vulkan, guarded);
        vkDestroyPipeline(vulkan.device, pipeline, nullptr);
    }

    vkDestroyPipelineLayout(vulkan.device, layout, nullptr);
    return test::finish();
}
