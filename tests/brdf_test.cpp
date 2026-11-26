// Checks shaders/material/brdf.slang against independent double-precision glTF 2.0 Appendix B / Heitz VNDF
// references (on-axis and off-axis), Helmholtz reciprocity, single-scattering white-furnace energy bounds,
// sampleBrdf/pdfBrdf/evalBrdf consistency (including view below the shading normal), envBrdfApprox, and degenerate
// inputs staying finite.
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
    constexpr uint32_t kSampleCount = 1u << 20;
    constexpr uint32_t kResultRows = 5;
    constexpr float kWorkedTolerance = 1e-3f;
    constexpr float kAnalyticTolerance = 2e-4f;
    constexpr float kIntegralTolerance = 0.02f;
    constexpr float kExactTolerance = 1e-4f;
    constexpr float kReciprocityTolerance = 5e-4f;
    constexpr float kEnergyCeiling = 1.005f;
    constexpr float kSkipPdf = -1.0f;
    constexpr double kPi = 3.14159265358979323846;

    struct Vec3
    {
        float x, y, z;
    };

    struct Vec3d
    {
        double x, y, z;
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
        Worked,       // compare f * NdotL and pdf at the case's l against hand-derived or analytic values
        Integrated,   // also compare the sampler mean against an independent uniform-hemisphere integral
        WhiteFurnace, // pure conductor f0 = 1: verify energy <= 1 and exact pdf/weight/reciprocity at low roughness
        FiniteOnly,   // degenerate input: no nan or inf; still verifies eval/sample/pdf consistency
    };

    struct TestCase
    {
        const char* name;
        BrdfCase input;
        Kind kind;
        Vec3 expected;
        float expectedPdf;
        float workedTolerance;
    };

    constexpr Vec3 kUp{0.0f, 0.0f, 1.0f};
    constexpr Vec3 kGold{1.0f, 0.766f, 0.336f};
    constexpr Vec3 kWhite{1.0f, 1.0f, 1.0f};
    constexpr Vec3 kBlack{0.0f, 0.0f, 0.0f};

    constexpr Vec3 gray(float value) { return {value, value, value}; }

    Vec3 normalized(Vec3 v)
    {
        const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        return {v.x / length, v.y / length, v.z / length};
    }

    Vec3d toDouble(Vec3 v) { return {v.x, v.y, v.z}; }
    double dot(Vec3d a, Vec3d b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

    Vec3d normalize(Vec3d v)
    {
        const double len = std::sqrt(dot(v, v));
        return {v.x / len, v.y / len, v.z / len};
    }

    // view at polar angle degrees from +z, azimuth azimuthDegrees from +x
    Vec3 dirAt(float polarDegrees, float azimuthDegrees = 0.0f)
    {
        const double polar = polarDegrees * kPi / 180.0;
        const double azimuth = azimuthDegrees * kPi / 180.0;
        return {static_cast<float>(std::sin(polar) * std::cos(azimuth)),
                static_cast<float>(std::sin(polar) * std::sin(azimuth)), static_cast<float>(std::cos(polar))};
    }

    // independent double-precision glTF 2.0 Appendix B + Heitz VNDF reference
    struct AnalyticReference
    {
        Vec3 fTimesNdotL;
        float pdf;
        Vec3 envBrdf;
    };

    AnalyticReference referenceGltfBrdf(const BrdfCase& c)
    {
        constexpr double kMinAlpha = 1e-3;
        constexpr double kMinNDotV = 1e-4;
        constexpr double kMinLobe = 0.1;

        const Vec3d n = normalize(toDouble(c.n));
        const Vec3d vRaw = normalize(toDouble(c.v));
        const Vec3d l = normalize(toDouble(c.l));
        const double nDotVRaw = dot(n, vRaw);
        const Vec3d v = nDotVRaw >= kMinNDotV
            ? vRaw
            : normalize({vRaw.x + n.x * (kMinNDotV - nDotVRaw), vRaw.y + n.y * (kMinNDotV - nDotVRaw),
                         vRaw.z + n.z * (kMinNDotV - nDotVRaw)});
        const double nDotV = std::max(dot(n, v), kMinNDotV);
        const double nDotL = dot(n, l);
        const double alpha = std::max(static_cast<double>(c.roughness) * c.roughness, kMinAlpha);
        const double a2 = alpha * alpha;

        // NVIDIA EnvBRDFApprox2 reference (Ray Tracing Gems ch. 32, m8 I14 F25)
        const double envAlpha = static_cast<double>(c.roughness) * c.roughness;
        const double noV = std::abs(nDotV);
        const std::array<double, 4> x{1.0, noV, noV * noV, noV * noV * noV};
        const std::array<double, 4> y{1.0, envAlpha, envAlpha * envAlpha, envAlpha * envAlpha * envAlpha};
        const auto m2x = [&](double a, double b, double c0, double d, double vx, double vy, double ux, double uy) {
            return (a * vx + b * vy) * ux + (c0 * vx + d * vy) * uy;
        };
        const auto m3x = [&](const std::array<double, 9>& m, double vx, double vy, double vz, double ux, double uy,
                             double uz) {
            return (m[0] * vx + m[1] * vy + m[2] * vz) * ux + (m[3] * vx + m[4] * vy + m[5] * vz) * uy +
                (m[6] * vx + m[7] * vy + m[8] * vz) * uz;
        };
        constexpr std::array<double, 9> kM2{1.0, 2.92338, 59.4188, 20.3225, -27.0302, 222.592, 121.563, 626.13,
                                            316.627};
        constexpr std::array<double, 9> kM4{1.0, 3.59685, -1.36772, 9.04401, -16.3174, 9.22949, 5.56589, 19.7886,
                                            -20.2123};
        double envBias = m2x(0.99044, -1.28514, 1.29678, -0.755907, x[0], x[1], y[0], y[1]) /
            m3x(kM2, x[0], x[1], x[3], y[0], y[1], y[3]);
        const double envScale = m2x(0.0365463, 3.32707, 9.0632, -9.04756, x[0], x[1], y[0], y[1]) /
            m3x(kM4, x[0], x[2], x[3], y[0], y[1], y[3]);
        const double maxF0 = std::max({static_cast<double>(c.f0.x), static_cast<double>(c.f0.y),
                                       static_cast<double>(c.f0.z)});
        envBias *= std::clamp(maxF0 * 50.0, 0.0, 1.0);
        const double clampedScale = std::max(0.0, envScale);
        const double clampedBias = std::max(0.0, envBias);
        const Vec3 envBrdf{static_cast<float>(c.f0.x * clampedScale + clampedBias),
                           static_cast<float>(c.f0.y * clampedScale + clampedBias),
                           static_cast<float>(c.f0.z * clampedScale + clampedBias)};

        if (nDotL <= 0.0) {
            return {kBlack, 0.0f, envBrdf};
        }

        const Vec3d h = normalize({l.x + v.x, l.y + v.y, l.z + v.z});
        const double nDotH = std::clamp(dot(n, h), 0.0, 1.0);
        const double vDotH = std::clamp(dot(v, h), 0.0, 1.0);

        const double dDenom = nDotH * nDotH * (a2 - 1.0) + 1.0;
        const double D = a2 / (kPi * dDenom * dDenom);
        const double ggxV = nDotL * std::sqrt(nDotV * nDotV * (1.0 - a2) + a2);
        const double ggxL = nDotV * std::sqrt(nDotL * nDotL * (1.0 - a2) + a2);
        const double V = 0.5 / std::max(ggxV + ggxL, 1e-7);

        const double f90 = std::clamp(maxF0 * 50.0, 0.0, 1.0);
        const double pow5 = std::pow(std::clamp(1.0 - vDotH, 0.0, 1.0), 5.0);
        const double fx = c.f0.x + (f90 - c.f0.x) * pow5;
        const double fy = c.f0.y + (f90 - c.f0.y) * pow5;
        const double fz = c.f0.z + (f90 - c.f0.z) * pow5;

        const Vec3 fTimesNdotL{
            static_cast<float>(((1.0 - fx) * c.cDiff.x / kPi + fx * D * V) * nDotL),
            static_cast<float>(((1.0 - fy) * c.cDiff.y / kPi + fy * D * V) * nDotL),
            static_cast<float>(((1.0 - fz) * c.cDiff.z / kPi + fz * D * V) * nDotL),
        };

        const auto lum = [](double r, double g, double b) { return 0.2126 * r + 0.7152 * g + 0.0722 * b; };
        const double cDiffLum = lum(c.cDiff.x, c.cDiff.y, c.cDiff.z);
        double specProb = 0.0;
        if (f90 > 0.0) {
            specProb = 1.0;
            if (cDiffLum > 0.0) {
                const double pow5V = std::pow(std::clamp(1.0 - nDotV, 0.0, 1.0), 5.0);
                const double fVx = c.f0.x + (f90 - c.f0.x) * pow5V;
                const double fVy = c.f0.y + (f90 - c.f0.y) * pow5V;
                const double fVz = c.f0.z + (f90 - c.f0.z) * pow5V;
                const double specLum = lum(fVx, fVy, fVz);
                const double diffLum = lum((1.0 - fVx) * c.cDiff.x, (1.0 - fVy) * c.cDiff.y, (1.0 - fVz) * c.cDiff.z);
                specProb = std::clamp(specLum / (specLum + diffLum), kMinLobe, 1.0 - kMinLobe);
            }
        }
        const double specPdf = D / (2.0 * (nDotV + std::sqrt(a2 + (1.0 - a2) * nDotV * nDotV)));
        const double diffPdf = nDotL / kPi;
        const float pdf = static_cast<float>((1.0 - specProb) * diffPdf + specProb * specPdf);
        return {fTimesNdotL, pdf, envBrdf};
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
        return {name, makeCase(kUp, kUp, kUp, cDiff, f0, roughness), Kind::Worked, expected, expectedPdf,
                kWorkedTolerance};
    }

    TestCase offAxisAnalytic(const char* name, Vec3 n, Vec3 v, Vec3 l, Vec3 cDiff, Vec3 f0, float roughness)
    {
        const BrdfCase input = makeCase(n, v, l, cDiff, f0, roughness);
        const AnalyticReference ref = referenceGltfBrdf(input);
        return {name, input, Kind::Worked, ref.fTimesNdotL, ref.pdf, kAnalyticTolerance};
    }

    TestCase integrated(const char* name, Vec3 n, Vec3 v, Vec3 cDiff, Vec3 f0, float roughness)
    {
        return {name, makeCase(n, v, n, cDiff, f0, roughness), Kind::Integrated, {}, kSkipPdf, kAnalyticTolerance};
    }

    TestCase whiteFurnace(const char* name, Vec3 v, float roughness)
    {
        return {name, makeCase(kUp, v, kUp, kBlack, kWhite, roughness), Kind::WhiteFurnace, {}, kSkipPdf,
                kAnalyticTolerance};
    }

    TestCase finiteOnly(const char* name, Vec3 n, Vec3 v, Vec3 cDiff, Vec3 f0, float roughness)
    {
        return {name, makeCase(n, v, n, cDiff, f0, roughness), Kind::FiniteOnly, {}, kSkipPdf, kAnalyticTolerance};
    }

    std::vector<TestCase> buildCases()
    {
        const Vec3 tilted = normalized({0.4f, -0.3f, 0.8f});
        const float goldScale = 795.77f;
        return {
            // hand-derived worked numbers (docs/pbr_lighting_materials.html section 7)
            worked("white plastic r 0.5", gray(0.8f), gray(0.04f), 0.5f, gray(0.29540f), 0.41380f),
            worked("gold r 0.1", kBlack, kGold, 0.1f,
                   {kGold.x * goldScale, kGold.y * goldScale, kGold.z * goldScale}, kSkipPdf),
            worked("glTF default", kBlack, kWhite, 1.0f, gray(0.07958f), kSkipPdf),
            worked("dark rubber r 1", gray(0.05f), gray(0.04f), 1.0f, gray(0.01846f), kSkipPdf),

            // off-axis exact glTF Appendix B + Heitz VNDF checks (non-trivial Fresnel, Smith G2 and half-vector)
            offAxisAnalytic("off-axis plastic 60/35 deg r 0.4", kUp, dirAt(60.0f, 0.0f), dirAt(35.0f, 130.0f),
                            {0.7f, 0.25f, 0.1f}, gray(0.04f), 0.4f),
            offAxisAnalytic("off-axis grazing Fresnel 80/75 deg r 0.25", kUp, dirAt(80.0f, 0.0f), dirAt(75.0f, 180.0f),
                            gray(0.5f), gray(0.04f), 0.25f),
            offAxisAnalytic("off-axis gold 45/40 deg r 0.15", kUp, dirAt(45.0f, 20.0f), dirAt(40.0f, 200.0f), kBlack,
                            kGold, 0.15f),
            offAxisAnalytic("off-axis sharp glossy 30/31 deg r 0.05", kUp, dirAt(30.0f, 0.0f), dirAt(31.0f, 180.0f),
                            gray(0.3f), gray(0.04f), 0.05f),
            offAxisAnalytic("off-axis tilted normal r 0.5", tilted, normalized({-0.2f, 0.1f, 0.9f}),
                            normalized({0.6f, -0.5f, 0.6f}), gray(0.6f), gray(0.04f), 0.5f),
            offAxisAnalytic("off-axis zero-f0 grazing 80/75 deg r 0.25", kUp, dirAt(80.0f, 0.0f), dirAt(75.0f, 180.0f),
                            gray(0.5f), kBlack, 0.25f),

            // sampler distribution vs independent uniform-hemisphere integral
            integrated("plastic head-on r 0.5", kUp, kUp, gray(0.8f), gray(0.04f), 0.5f),
            integrated("zero-f0 grazing 80 deg r 0.3", kUp, dirAt(80.0f), gray(0.6f), kBlack, 0.3f),
            integrated("plastic 60 deg r 0.3", kUp, dirAt(60.0f), {0.7f, 0.2f, 0.1f}, gray(0.04f), 0.3f),
            integrated("plastic 85 deg r 0.6", kUp, dirAt(85.0f), gray(0.5f), gray(0.04f), 0.6f),
            integrated("gold 45 deg r 0.4", kUp, dirAt(45.0f), kBlack, kGold, 0.4f),
            integrated("metal 80 deg r 1", kUp, dirAt(80.0f), kBlack, gray(0.9f), 1.0f),
            integrated("tilted normal r 0.5", tilted, normalized({-0.2f, 0.1f, 0.9f}), gray(0.6f), gray(0.04f), 0.5f),
            integrated("south-pole normal r 0.5", {0.0f, 0.0f, -1.0f}, normalized({0.3f, 0.2f, -0.9f}), gray(0.6f),
                       gray(0.04f), 0.5f),

            // single-scattering GGX white-furnace energy conservation (including sharp roughness < 0.3)
            whiteFurnace("white furnace 30 deg r 0.05", dirAt(30.0f), 0.05f),
            whiteFurnace("white furnace 60 deg r 0.1", dirAt(60.0f), 0.1f),
            whiteFurnace("white furnace 45 deg r 0.5", dirAt(45.0f), 0.5f),
            whiteFurnace("white furnace 75 deg r 1.0", dirAt(75.0f), 1.0f),

            // degenerate inputs: finite + eval/pdf/sample agreement even when view sits below the shading normal
            finiteOnly("mirror head-on r 0", kUp, kUp, kBlack, kWhite, 0.0f),
            finiteOnly("mirror plastic grazing r 0", kUp, dirAt(89.9f), gray(0.5f), gray(0.04f), 0.0f),
            finiteOnly("view below the shading normal", kUp, dirAt(100.0f), gray(0.5f), gray(0.04f), 0.5f),
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
                const float* stats, const float* envBrdf)
    {
        std::printf("  %s: f*cos (%.5f %.5f %.5f) pdf %.5f, mean weight (%.4f %.4f %.4f), integral (%.4f %.4f %.4f), "
                    "pdf integral %.4f, invalid %.4f, recip %.2e\n",
                    testCase.name, evaluated[0], evaluated[1], evaluated[2], evaluated[3], sampled[0], sampled[1],
                    sampled[2], uniform[0], uniform[1], uniform[2], uniform[3], stats[1], stats[3]);

        check(stats[2] == 0.0f, "  samples are finite and above the surface");
        check(std::isfinite(evaluated[0]) && std::isfinite(evaluated[1]) && std::isfinite(evaluated[2]) &&
                  std::isfinite(evaluated[3]),
              "  eval and pdf are finite");
        check(sampled[3] <= kExactTolerance, "  pdfBrdf matches sampleBrdf");
        check(stats[0] <= kExactTolerance, "  weight is f * NdotL / pdf");
        if (testCase.kind == Kind::FiniteOnly) {
            return;
        }
        check(stats[3] <= kReciprocityTolerance, "  evalBrdf satisfies Helmholtz reciprocity");
        const AnalyticReference ref = referenceGltfBrdf(testCase.input);
        check(isClose(envBrdf, ref.envBrdf, kAnalyticTolerance), "  envBrdfApprox matches Karis fit");
        if (testCase.kind == Kind::Worked) {
            check(isClose(evaluated, testCase.expected, testCase.workedTolerance),
                  "  f * NdotL matches the analytic value");
            if (testCase.expectedPdf != kSkipPdf) {
                check(isClose(evaluated[3], testCase.expectedPdf, testCase.workedTolerance),
                      "  pdf matches the analytic value");
            }
            return;
        }
        if (testCase.kind == Kind::WhiteFurnace) {
            check(sampled[0] > 0.0f && sampled[0] <= kEnergyCeiling && sampled[1] <= kEnergyCeiling &&
                      sampled[2] <= kEnergyCeiling,
                  "  single-scattering white-furnace albedo in (0, 1]");
            return;
        }
        check(isClose(sampled, {uniform[0], uniform[1], uniform[2]}, kIntegralTolerance),
              "  mean weight equals the uniform integral");
        // reflections lost below the horizon are the only missing mass
        check(isClose(uniform[3] + stats[1], 1.0f, kIntegralTolerance), "  pdf integral + invalid fraction is 1");
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
            verify(cases[i], row, row + 4, row + 8, row + 12, row + 16);
        }
        test::destroyHostBuffer(vulkan, inputs);
        test::destroyHostBuffer(vulkan, results);
        vkDestroyPipeline(vulkan.device, pipeline, nullptr);
    }

    vkDestroyPipelineLayout(vulkan.device, layout, nullptr);
    return test::finish();
}
