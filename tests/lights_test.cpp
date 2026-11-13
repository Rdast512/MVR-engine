// Checks shaders/lighting/lights.slang against KHR_lights_punctual: directional radiance in lux, the inverse-square
// falloff with the range window, the spot cone sign (on axis is +1, behind the spot is dark), hand-derived numbers,
// and a light at the shading point staying finite.
#include "test_support.hpp"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using test::check;

namespace
{
    constexpr uint32_t kResultRows = 2;
    constexpr float kTolerance = 1e-4f;
    constexpr double kPi = 3.14159265358979323846;

    struct Vec3
    {
        float x, y, z;
    };

    // scalar layout: must match GpuLight in src/core/types.hpp (checked by tests/layout_test.cpp)
    struct LightRow
    {
        Vec3 position;
        float range;
        Vec3 direction;
        float intensity;
        Vec3 color;
        uint32_t type;
        float innerCone;
        float outerCone;
        uint32_t instanceIndex;
        float pad0;
    };
    static_assert(sizeof(LightRow) == 64);
    static_assert(offsetof(LightRow, type) == 44);

    // scalar layout: must match LightCase in tests/shaders/lights_numbers.slang
    struct LightCase
    {
        LightRow light;
        Vec3 p;
        float pad0;
    };
    static_assert(sizeof(LightCase) == 80);

    // scalar layout: must match LightPush in tests/shaders/lights_numbers.slang
    struct LightPush
    {
        VkDeviceAddress cases;
        VkDeviceAddress results;
        uint32_t caseCount;
    };
    static_assert(sizeof(LightPush) == 24);

    // GpuLightType
    constexpr uint32_t kDirectional = 0;
    constexpr uint32_t kPoint = 1;
    constexpr uint32_t kSpot = 2;

    struct TestCase
    {
        const char* name;
        LightCase input;
        Vec3 expectedRadiance;
        Vec3 expectedL;
        float expectedDistance;
    };

    constexpr Vec3 kOrigin{0.0f, 0.0f, 0.0f};
    constexpr Vec3 kUp{0.0f, 0.0f, 1.0f};
    constexpr Vec3 kDown{0.0f, 0.0f, -1.0f};
    constexpr Vec3 kWhite{1.0f, 1.0f, 1.0f};

    double radians(double degrees) { return degrees * kPi / 180.0; }

    Vec3 scaled(Vec3 v, float s) { return {v.x * s, v.y * s, v.z * s}; }

    // KHR_lights_punctual reference, written from the extension text in double precision
    double khronosAttenuation(double d, double range, double cosToAxis, uint32_t type, double inner, double outer)
    {
        double attenuation = 1.0 / std::fmax(d * d, 1e-4);
        if (range > 0.0) {
            attenuation *= std::fmax(std::fmin(1.0 - std::pow(d / range, 4.0), 1.0), 0.0);
        }
        if (type == kSpot) {
            const double scale = 1.0 / std::fmax(0.001, std::cos(inner) - std::cos(outer));
            const double offset = -std::cos(outer) * scale;
            const double t = std::fmax(std::fmin(cosToAxis * scale + offset, 1.0), 0.0);
            attenuation *= t * t;
        }
        return attenuation;
    }

    LightRow makeLight(uint32_t type, Vec3 position, Vec3 direction, Vec3 color, float intensity, float range,
                       float innerCone = 0.0f, float outerCone = static_cast<float>(kPi / 4.0))
    {
        LightRow light{};
        light.type = type;
        light.position = position;
        light.direction = direction;
        light.color = color;
        light.intensity = intensity;
        light.range = range;
        light.innerCone = innerCone;
        light.outerCone = outerCone;
        return light;
    }

    TestCase directional(const char* name, Vec3 direction, Vec3 color, float intensity)
    {
        const LightRow light = makeLight(kDirectional, kOrigin, direction, color, intensity, 0.0f);
        return {name,
                {light, kOrigin, 0.0f},
                scaled(color, intensity),
                {-direction.x, -direction.y, -direction.z},
                std::numeric_limits<float>::max()};
    }

    // light 2 m above the origin; p at polar angle degrees off the light's down axis, 2 m away
    TestCase local(const char* name, const LightRow& lightAbove, double degrees, double distance)
    {
        LightRow light = lightAbove;
        light.position = {0.0f, 0.0f, 2.0f};
        const double angle = radians(degrees);
        const Vec3 p{static_cast<float>(distance * std::sin(angle)), 0.0f,
                     static_cast<float>(2.0 - distance * std::cos(angle))};
        const Vec3 l{static_cast<float>(-std::sin(angle)), 0.0f, static_cast<float>(std::cos(angle))};
        // cosToAxis = dot(spotDir, -l)
        const double cosToAxis = -(light.direction.x * l.x + light.direction.y * l.y + light.direction.z * l.z);
        const double attenuation =
            khronosAttenuation(distance, light.range, cosToAxis, light.type, light.innerCone, light.outerCone);
        return {name,
                {light, p, 0.0f},
                scaled(light.color, static_cast<float>(light.intensity * attenuation)),
                l,
                static_cast<float>(distance)};
    }

    std::vector<TestCase> buildCases()
    {
        const LightRow point = makeLight(kPoint, kOrigin, kDown, kWhite, 4.0f, 0.0f);
        const LightRow pointRange4 = makeLight(kPoint, kOrigin, kDown, kWhite, 4.0f, 4.0f);
        const LightRow spot = makeLight(kSpot, kOrigin, kDown, {1.0f, 0.5f, 0.25f}, 4.0f, 0.0f);
        const LightRow spotUp = makeLight(kSpot, kOrigin, kUp, kWhite, 4.0f, 0.0f);
        const LightRow spotSoft = makeLight(kSpot, kOrigin, kDown, kWhite, 4.0f, 0.0f,
                                            static_cast<float>(radians(20.0)), static_cast<float>(radians(40.0)));
        const LightRow spotHard = makeLight(kSpot, kOrigin, kDown, kWhite, 4.0f, 0.0f,
                                            static_cast<float>(radians(30.0)), static_cast<float>(radians(30.0)));

        std::vector<TestCase> cases{
            directional("directional, lux not attenuated", kDown, {1.0f, 0.5f, 0.25f}, 3.0f),
            directional("directional, slanted", {0.6f, 0.0f, -0.8f}, kWhite, 100000.0f),

            local("point, inverse square", point, 0.0, 2.0),
            local("point, off axis", point, 70.0, 2.0),
            local("point, range window", pointRange4, 0.0, 2.0),
            local("point, at the range", pointRange4, 0.0, 4.0),
            local("point, past the range", pointRange4, 0.0, 5.0),
            local("point, closer than 1 cm", point, 0.0, 0.005),

            local("spot, on axis", spot, 0.0, 2.0),
            local("spot, 30 deg of 45", spot, 30.0, 2.0),
            local("spot, outside the cone", spot, 50.0, 2.0),
            local("spot, behind a spot facing away", spotUp, 0.0, 2.0),
            local("spot, inner 20 outer 40 at 30 deg", spotSoft, 30.0, 2.0),
            local("spot, inner equals outer, inside", spotHard, 29.0, 2.0),
        };

        // hand-derived: 4 cd / 2² = 1; window 1 - 0.5^4 = 0.9375; on axis cone = 1
        cases[2].expectedRadiance = kWhite;
        cases[4].expectedRadiance = {0.9375f, 0.9375f, 0.9375f};
        cases[8].expectedRadiance = {1.0f, 0.5f, 0.25f};
        return cases;
    }

    bool isClose(float actual, float expected)
    {
        return std::fabs(actual - expected) <= kTolerance * std::fmax(std::fabs(expected), 1.0f);
    }

    bool isClose(const float* actual, Vec3 expected)
    {
        return isClose(actual[0], expected.x) && isClose(actual[1], expected.y) && isClose(actual[2], expected.z);
    }

    void verify(const TestCase& testCase, const float* radiance, const float* l)
    {
        std::printf("  %s: radiance (%.5f %.5f %.5f) distance %g, l (%.4f %.4f %.4f)\n", testCase.name, radiance[0],
                    radiance[1], radiance[2], radiance[3], l[0], l[1], l[2]);
        check(isClose(radiance, testCase.expectedRadiance), "  radiance matches KHR_lights_punctual");
        check(isClose(l, testCase.expectedL), "  l points toward the light");
        check(radiance[3] == testCase.expectedDistance || isClose(radiance[3], testCase.expectedDistance),
              "  distance is the shadow ray tMax");
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

    const VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(LightPush)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    vkCreatePipelineLayout(vulkan.device, &layoutInfo, nullptr, &layout);

    const VkPipeline pipeline = test::createComputePipeline(
        vulkan, layout, std::string(LIGHTS_TEST_SHADER_DIR) + "/lightsMain.spv", "lightsMain");
    if (check(pipeline != VK_NULL_HANDLE, "lights pipeline created")) {
        const std::vector<TestCase> cases = buildCases();
        const auto caseCount = static_cast<uint32_t>(cases.size());
        test::HostBuffer inputs = test::createHostBuffer(vulkan, caseCount * sizeof(LightCase));
        test::HostBuffer results = test::createHostBuffer(vulkan, caseCount * kResultRows * 4 * sizeof(float));
        auto* inputData = static_cast<LightCase*>(inputs.mapped);
        for (uint32_t i = 0; i < caseCount; ++i) {
            inputData[i] = cases[i].input;
        }

        const LightPush push{inputs.address, results.address, caseCount};
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
        check(vulkan.submit(cmd), "lights dispatch submitted");
        vulkan.waitIdle();

        const auto* rows = static_cast<const float*>(results.mapped);
        for (uint32_t i = 0; i < caseCount; ++i) {
            const float* row = rows + i * kResultRows * 4;
            verify(cases[i], row, row + 4);
        }
        test::destroyHostBuffer(vulkan, inputs);
        test::destroyHostBuffer(vulkan, results);
        vkDestroyPipeline(vulkan.device, pipeline, nullptr);
    }

    vkDestroyPipelineLayout(vulkan.device, layout, nullptr);
    return test::finish();
}
