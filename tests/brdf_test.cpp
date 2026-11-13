// Checks shaders/material/brdf.slang: the worked numbers of docs/pbr_lighting_materials.html section 7, that pdfBrdf and
// the weight match sampleBrdf exactly, that sampleBrdf draws from the distribution pdfBrdf describes (its mean weight
// equals an independent uniform-hemisphere integral of f * NdotL), and that degenerate inputs stay finite.
#include "test_support.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

using test::check;

namespace
{
    constexpr uint32_t kSampleCount = 1u << 20;
    constexpr uint32_t kResultRows = 4;
    constexpr float kWorkedTolerance = 1e-3f;
    constexpr float kIntegralTolerance = 0.02f;
    constexpr float kExactTolerance = 1e-4f;
    constexpr float kSkipPdf = -1.0f;

    struct Vec3
    {
        float x, y, z;
    };

    // scalar layout: must match BrdfCase in tests/shaders/brdf_numbers.slang
    struct BrdfCase
    {
        Vec3 n;
        float roughness;
        Vec3 v;
        float pad0;
        Vec3 l;
        float pad1;
        Vec3 cDiff;
        float pad2;
        Vec3 f0;
        float pad3;
    };
    static_assert(sizeof(BrdfCase) == 80);
    static_assert(offsetof(BrdfCase, f0) == 64);

    // scalar layout: must match BrdfPush in tests/shaders/brdf_numbers.slang
    struct BrdfPush
    {
        VkDeviceAddress cases;
        VkDeviceAddress results;
        uint32_t caseCount;
        uint32_t sampleCount;
    };
    static_assert(sizeof(BrdfPush) == 24);

    enum class Kind
    {
        Worked,     // compare f * NdotL and pdf at the case's l against hand-derived values
        Integrated, // also compare the estimators against each other
        FiniteOnly, // degenerate input: no nan or inf
    };

    struct TestCase
    {
        const char* name;
        BrdfCase input;
        Kind kind;
        Vec3 expected;
        float expectedPdf;
    };

    constexpr Vec3 kUp{0.0f, 0.0f, 1.0f};
    constexpr Vec3 kGold{1.0f, 0.766f, 0.336f};
    constexpr Vec3 kBlack{0.0f, 0.0f, 0.0f};

    constexpr Vec3 gray(float value) { return {value, value, value}; }

    Vec3 normalized(Vec3 v)
    {
        const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        return {v.x / length, v.y / length, v.z / length};
    }

    // view at polar angle degrees from +z
    Vec3 viewAt(float degrees)
    {
        const float radians = degrees * 3.14159265f / 180.0f;
        return {std::sin(radians), 0.0f, std::cos(radians)};
    }

    BrdfCase makeCase(Vec3 n, Vec3 v, Vec3 l, Vec3 cDiff, Vec3 f0, float roughness)
    {
        BrdfCase input{};
        input.n = n;
        input.v = v;
        input.l = l;
        input.cDiff = cDiff;
        input.f0 = f0;
        input.roughness = roughness;
        return input;
    }

    TestCase worked(const char* name, Vec3 cDiff, Vec3 f0, float roughness, Vec3 expected, float expectedPdf)
    {
        return {name, makeCase(kUp, kUp, kUp, cDiff, f0, roughness), Kind::Worked, expected, expectedPdf};
    }

    TestCase integrated(const char* name, Vec3 n, Vec3 v, Vec3 cDiff, Vec3 f0, float roughness)
    {
        return {name, makeCase(n, v, n, cDiff, f0, roughness), Kind::Integrated, {}, kSkipPdf};
    }

    TestCase finiteOnly(const char* name, Vec3 n, Vec3 v, Vec3 cDiff, Vec3 f0, float roughness)
    {
        return {name, makeCase(n, v, n, cDiff, f0, roughness), Kind::FiniteOnly, {}, kSkipPdf};
    }

    std::vector<TestCase> buildCases()
    {
        const Vec3 tilted = normalized({0.4f, -0.3f, 0.8f});
        const float goldScale = 795.77f;
        return {
            // f * NdotL from the section 7 table; the plastic pdf is hand-derived: 0.1 (clamped) * 1.2732 + 0.9 * 0.3183
            worked("white plastic r 0.5", gray(0.8f), gray(0.04f), 0.5f, gray(0.29540f), 0.41380f),
            worked("gold r 0.1", kBlack, kGold, 0.1f,
                   {kGold.x * goldScale, kGold.y * goldScale, kGold.z * goldScale}, kSkipPdf),
            worked("glTF default", kBlack, gray(1.0f), 1.0f, gray(0.07958f), kSkipPdf),
            worked("dark rubber r 1", gray(0.05f), gray(0.04f), 1.0f, gray(0.01846f), kSkipPdf),

            integrated("plastic head-on r 0.5", kUp, kUp, gray(0.8f), gray(0.04f), 0.5f),
            integrated("plastic 60 deg r 0.3", kUp, viewAt(60.0f), {0.7f, 0.2f, 0.1f}, gray(0.04f), 0.3f),
            integrated("plastic 85 deg r 0.6", kUp, viewAt(85.0f), gray(0.5f), gray(0.04f), 0.6f),
            integrated("gold 45 deg r 0.4", kUp, viewAt(45.0f), kBlack, kGold, 0.4f),
            integrated("metal 80 deg r 1", kUp, viewAt(80.0f), kBlack, gray(0.9f), 1.0f),
            integrated("tilted normal r 0.5", tilted, normalized({-0.2f, 0.1f, 0.9f}), gray(0.6f), gray(0.04f), 0.5f),
            integrated("south-pole normal r 0.5", {0.0f, 0.0f, -1.0f}, normalized({0.3f, 0.2f, -0.9f}), gray(0.6f),
                       gray(0.04f), 0.5f),

            finiteOnly("mirror head-on r 0", kUp, kUp, kBlack, gray(1.0f), 0.0f),
            finiteOnly("mirror plastic grazing r 0", kUp, viewAt(89.9f), gray(0.5f), gray(0.04f), 0.0f),
            finiteOnly("view below the shading normal", kUp, viewAt(100.0f), gray(0.5f), gray(0.04f), 0.5f),
            finiteOnly("view opposite the normal", kUp, {0.0f, 0.0f, -1.0f}, gray(0.5f), gray(0.04f), 0.5f),
            finiteOnly("black material", kUp, kUp, kBlack, kBlack, 0.5f),
        };
    }

    bool isClose(float actual, float expected, float tolerance)
    {
        return std::fabs(actual - expected) <= tolerance * std::fmax(std::fabs(expected), 1e-3f);
    }

    bool isClose(const float* actual, Vec3 expected, float tolerance)
    {
        return isClose(actual[0], expected.x, tolerance) && isClose(actual[1], expected.y, tolerance) &&
            isClose(actual[2], expected.z, tolerance);
    }

    void verify(const TestCase& testCase, const float* evaluated, const float* sampled, const float* uniform,
                const float* stats)
    {
        std::printf("  %s: f*cos (%.5f %.5f %.5f) pdf %.5f, mean weight (%.4f %.4f %.4f), integral (%.4f %.4f %.4f), "
                    "pdf integral %.4f, invalid %.4f\n",
                    testCase.name, evaluated[0], evaluated[1], evaluated[2], evaluated[3], sampled[0], sampled[1],
                    sampled[2], uniform[0], uniform[1], uniform[2], uniform[3], stats[1]);

        check(stats[2] == 0.0f, "  samples are finite and above the surface");
        if (testCase.kind == Kind::FiniteOnly) {
            check(std::isfinite(evaluated[0]) && std::isfinite(evaluated[3]), "  eval and pdf are finite");
            return;
        }
        check(sampled[3] <= kExactTolerance, "  pdfBrdf matches sampleBrdf");
        check(stats[0] <= kExactTolerance, "  weight is f * NdotL / pdf");
        if (testCase.kind == Kind::Worked) {
            check(isClose(evaluated, testCase.expected, kWorkedTolerance), "  f * NdotL matches the worked number");
            if (testCase.expectedPdf != kSkipPdf) {
                check(isClose(evaluated[3], testCase.expectedPdf, kWorkedTolerance), "  pdf matches the worked number");
            }
            return;
        }
        check(isClose(sampled, {uniform[0], uniform[1], uniform[2]}, kIntegralTolerance),
              "  mean weight equals the uniform integral");
        // reflections lost below the horizon are the only missing mass
        check(isClose(uniform[3] + stats[1], 1.0f, kIntegralTolerance), "  pdf integral + invalid fraction is 1");
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

    const VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(BrdfPush)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    vkCreatePipelineLayout(vulkan.device, &layoutInfo, nullptr, &layout);

    const VkPipeline pipeline =
        test::createComputePipeline(vulkan, layout, std::string(BRDF_TEST_SHADER_DIR) + "/brdfMain.spv", "brdfMain");
    if (check(pipeline != VK_NULL_HANDLE, "brdf pipeline created")) {
        const std::vector<TestCase> cases = buildCases();
        const auto caseCount = static_cast<uint32_t>(cases.size());
        test::HostBuffer inputs = test::createHostBuffer(vulkan, caseCount * sizeof(BrdfCase));
        test::HostBuffer results = test::createHostBuffer(vulkan, caseCount * kResultRows * 4 * sizeof(float));
        auto* inputData = static_cast<BrdfCase*>(inputs.mapped);
        for (uint32_t i = 0; i < caseCount; ++i) {
            inputData[i] = cases[i].input;
        }

        const BrdfPush push{inputs.address, results.address, caseCount, kSampleCount};
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
        check(vulkan.submit(cmd), "brdf dispatch submitted");
        vulkan.waitIdle();

        const auto* rows = static_cast<const float*>(results.mapped);
        for (uint32_t i = 0; i < caseCount; ++i) {
            const float* row = rows + i * kResultRows * 4;
            verify(cases[i], row, row + 4, row + 8, row + 12);
        }
        test::destroyHostBuffer(vulkan, inputs);
        test::destroyHostBuffer(vulkan, results);
        vkDestroyPipeline(vulkan.device, pipeline, nullptr);
    }

    vkDestroyPipelineLayout(vulkan.device, layout, nullptr);
    return test::finish();
}
