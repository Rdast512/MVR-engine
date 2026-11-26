// Checks RTXDI bridge GI math and guards directly on the GPU: LogLuv GI reservoir packing across 8 orders of
// magnitude, RAB_ValidateGISampleWithJacobian bounds against NVIDIA RAB_SpatialHelpers.hlsli, shadeGiSample /
// RAB_GetGISampleTargetPdfForSurface against an independent double-precision glTF BRDF reference, CheckedBuffer
// out-of-bounds guards (considerations C-F3-1), and the compact light round trip of every light type within f16
// precision (spec/m7/f1_light_presampling.md).
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
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
        VkDeviceAddress lightInputs;
        VkDeviceAddress lightOutputs;
        VkDeviceAddress compactedFlags;
        VkDeviceAddress compactRows;
        uint32_t lightCount;
        uint32_t pad0;
    };
    static_assert(sizeof(RtxdiMathPush) == 96);

    // GpuLight / GpuTriangleLight of shaders/types.slang: 16 words, the type at word 11 in both
    struct LightRow
    {
        std::array<float, 16> words{};

        void setVec3(size_t word, float x, float y, float z)
        {
            words[word] = x;
            words[word + 1] = y;
            words[word + 2] = z;
        }
        void setUint(size_t word, uint32_t value) { std::memcpy(&words[word], &value, sizeof(value)); }
        [[nodiscard]] uint32_t getUint(size_t word) const
        {
            uint32_t value = 0;
            std::memcpy(&value, &words[word], sizeof(value));
            return value;
        }
    };
    static_assert(sizeof(LightRow) == 64);

    // LightType and GpuTriangleLight words
    constexpr uint32_t kPointLight = 1;
    constexpr uint32_t kSpotLight = 2;
    constexpr uint32_t kTriangleLight = 3;
    constexpr size_t kTypeWord = 11;
    constexpr size_t kV0Word = 0;
    constexpr size_t kEdge1Word = 3;
    constexpr size_t kEdge2Word = 6;
    constexpr size_t kFlagsWord = 9;
    constexpr size_t kRadianceWord = 12;
    // GpuLight words
    constexpr size_t kPositionWord = 0;
    constexpr size_t kRangeWord = 3;
    constexpr size_t kIntensityWord = 7;
    constexpr size_t kColorWord = 8;
    // f16 keeps 11 significant bits; the octahedral unorm16 directions add ~5e-5 rad
    constexpr float kHalfTolerance = 1.0e-3f;

    LightRow triangleRow(std::array<float, 3> v0, std::array<float, 3> e1, std::array<float, 3> e2,
                         std::array<float, 3> radiance, uint32_t flags)
    {
        LightRow row;
        row.setVec3(kV0Word, v0[0], v0[1], v0[2]);
        row.setVec3(kEdge1Word, e1[0], e1[1], e1[2]);
        row.setVec3(kEdge2Word, e2[0], e2[1], e2[2]);
        row.setUint(kFlagsWord, flags);
        row.setUint(kTypeWord, kTriangleLight);
        row.setVec3(kRadianceWord, radiance[0], radiance[1], radiance[2]);
        return row;
    }

    LightRow punctualRow(uint32_t type, std::array<float, 3> position, float range, float intensity,
                         std::array<float, 3> color)
    {
        LightRow row;
        row.setVec3(kPositionWord, position[0], position[1], position[2]);
        row.words[kRangeWord] = range;
        row.setVec3(4, 0.0f, -1.0f, 0.0f);
        row.words[kIntensityWord] = intensity;
        row.setVec3(kColorWord, color[0], color[1], color[2]);
        row.setUint(kTypeWord, type);
        row.words[12] = 0.2f;
        row.words[13] = 0.6f;
        return row;
    }

    float vec3Length(const LightRow& row, size_t word)
    {
        return std::sqrt(row.words[word] * row.words[word] + row.words[word + 1] * row.words[word + 1] +
                         row.words[word + 2] * row.words[word + 2]);
    }

    // largest distance between matching corners (v0, v0 + edge1, v0 + edge2)
    float triangleCornerError(const LightRow& in, const LightRow& out)
    {
        float error = 0.0f;
        for (const size_t edge : {size_t{0}, kEdge1Word, kEdge2Word}) {
            float squared = 0.0f;
            for (size_t axis = 0; axis < 3; ++axis) {
                const float a = in.words[kV0Word + axis] + (edge != 0 ? in.words[edge + axis] : 0.0f);
                const float b = out.words[kV0Word + axis] + (edge != 0 ? out.words[edge + axis] : 0.0f);
                squared += (a - b) * (a - b);
            }
            error = std::max(error, std::sqrt(squared));
        }
        return error;
    }

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
            float expectedValue;
        };
        // M8 I2 F20: unclamped symmetric [1/3, 3] rejection (preserves multi-hop Jacobian telescoping)
        const std::vector<JacobianCase> jacobianCases{
            {0.0f, false, 0.0f},
            {0.25f, false, 0.25f},
            {0.33f, false, 0.33f},
            {1.0f / 3.0f, true, 1.0f / 3.0f},
            {0.5f, true, 0.5f},
            {1.0f, true, 1.0f},
            {2.5f, true, 2.5f},
            {3.0f, true, 3.0f},
            {3.01f, false, 3.01f},
            {10.01f, false, 10.01f},
        };

        test::HostBuffer radIn = test::createHostBuffer(vulkan, radianceInputs.size() * sizeof(Vec4));
        test::HostBuffer radOut = test::createHostBuffer(vulkan, radianceInputs.size() * sizeof(Vec4));
        test::HostBuffer jacIn = test::createHostBuffer(vulkan, jacobianCases.size() * sizeof(float));
        test::HostBuffer jacOut = test::createHostBuffer(vulkan, jacobianCases.size() * sizeof(Vec4));
        test::HostBuffer shadeOut = test::createHostBuffer(vulkan, 4 * sizeof(Vec4));
        test::HostBuffer guarded = test::createHostBuffer(vulkan, 4 * sizeof(uint32_t));

        struct CompactCase
        {
            LightRow row;
            bool isCompacted;
        };
        const std::vector<CompactCase> compactCases{
            {triangleRow({1.0f, 2.0f, 3.0f}, {0.5f, 0.1f, -0.2f}, {-0.1f, 0.4f, 0.3f}, {12.5f, 3.0f, 0.25f}, 0u),
             true},
            // double-sided and mirrored, far from the origin
            {triangleRow({-40.0f, 5.0f, 120.0f}, {2.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 3.0f}, {1000.0f, 500.0f, 0.001f},
                         3u),
             true},
            // degenerate: a zero-length edge keeps any direction
            {triangleRow({0.25f, -0.5f, 0.75f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, 0u), true},
            {punctualRow(kPointLight, {3.0f, -1.0f, 2.5f}, 7.5f, 150.0f, {1.0f, 0.8f, 0.6f}), true},
            // range 0: no range window
            {punctualRow(kPointLight, {-12.0f, 4.0f, 0.5f}, 0.0f, 2.5e5f, {0.3f, 0.6f, 0.9f}), true},
            // shaped: loaded from the world row, as the RTXDI sample's spot lights
            {punctualRow(kSpotLight, {0.0f, 3.0f, 0.0f}, 10.0f, 40.0f, {1.0f, 1.0f, 1.0f}), false},
            // past the f16 range
            {triangleRow({0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {7.0e4f, 1.0f, 1.0f}, 0u), false},
            // M8.1 BUG-A4: below f16 normal range falls back to fp32 worldLights
            {triangleRow({0.0f, 0.0f, 0.0f}, {1.0e-5f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, 0u), false},
            {punctualRow(kPointLight, {0.0f, 1.0f, 0.0f}, 5.0f, 10.0f, {1.0e-5f, 1.0f, 1.0f}), false},
        };
        const auto lightCount = static_cast<uint32_t>(compactCases.size());
        test::HostBuffer lightIn = test::createHostBuffer(vulkan, lightCount * sizeof(LightRow));
        test::HostBuffer lightOut = test::createHostBuffer(vulkan, lightCount * sizeof(LightRow));
        test::HostBuffer compactedOut = test::createHostBuffer(vulkan, lightCount * sizeof(uint32_t));
        test::HostBuffer compactRows = test::createHostBuffer(vulkan, lightCount * 8 * sizeof(uint32_t));
        auto* lightInPtr = static_cast<LightRow*>(lightIn.mapped);
        for (size_t i = 0; i < compactCases.size(); ++i) {
            lightInPtr[i] = compactCases[i].row;
        }

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
            .lightInputs = lightIn.address,
            .lightOutputs = lightOut.address,
            .compactedFlags = compactedOut.address,
            .compactRows = compactRows.address,
            .lightCount = lightCount,
            .pad0 = 0,
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

        // 2. Jacobian validation bounds (unclamped symmetric [1/3, 3], M8 I2 F20)
        const auto* jacOutPtr = static_cast<const Vec4*>(jacOut.mapped);
        for (size_t i = 0; i < jacobianCases.size(); ++i) {
            const bool valid = jacOutPtr[i].x > 0.5f;
            const bool ok = valid == jacobianCases[i].expectedValid &&
                (!valid || relClose(jacOutPtr[i].y, jacobianCases[i].expectedValue, 1e-5f));
            check(ok, "  RAB_ValidateGISampleWithJacobian matches symmetric [1/3, 3] bounds");
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

        // 5. compact light round trip
        const auto* lightOutPtr = static_cast<const LightRow*>(lightOut.mapped);
        const auto* compactedPtr = static_cast<const uint32_t*>(compactedOut.mapped);
        for (size_t i = 0; i < compactCases.size(); ++i) {
            const LightRow& in = compactCases[i].row;
            const LightRow& out = lightOutPtr[i];
            const bool isCompacted = compactedPtr[i] != 0;
            if (!check(isCompacted == compactCases[i].isCompacted, "  compact light accepted for its type")) {
                continue;
            }
            if (!isCompacted) {
                continue;
            }
            const uint32_t type = in.getUint(kTypeWord);
            bool isClose = out.getUint(kTypeWord) == type;
            for (size_t axis = 0; axis < 3; ++axis) {
                const size_t colorWord = (type == kTriangleLight ? kRadianceWord : kColorWord) + axis;
                isClose = isClose && relClose(out.words[colorWord], in.words[colorWord], kHalfTolerance);
            }
            if (type == kTriangleLight) {
                const float size = vec3Length(in, kEdge1Word) + vec3Length(in, kEdge2Word);
                const float error = triangleCornerError(in, out);
                std::printf("  compact triangle[%zu]: corner error %.3g of edge sum %.3g\n", i, error, size);
                isClose = isClose && out.getUint(kFlagsWord) == in.getUint(kFlagsWord) &&
                    error <= kHalfTolerance * size + 1.0e-6f * (1.0f + vec3Length(in, kV0Word));
            } else {
                for (const size_t word : {kPositionWord, kPositionWord + 1, kPositionWord + 2, kRangeWord,
                                          kIntensityWord}) {
                    isClose = isClose && out.words[word] == in.words[word];
                }
            }
            check(isClose, "  compact light round trip within f16 precision");
        }

        test::destroyHostBuffer(vulkan, lightIn);
        test::destroyHostBuffer(vulkan, lightOut);
        test::destroyHostBuffer(vulkan, compactedOut);
        test::destroyHostBuffer(vulkan, compactRows);
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
