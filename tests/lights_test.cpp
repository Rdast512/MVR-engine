// Checks shaders/lighting/lights.slang and core/frame.slang::toWorldLight against KHR_lights_punctual: directional
// radiance in lux, inverse-square falloff with the range window, spot cone sign, exact zero outside range/cone,
// exact d == 0 finiteness, and node transforms keeping intensity/range/cone unscaled with unit direction.
#include "test_support.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using test::check;

namespace
{
    constexpr uint32_t kResultRows = 4;
    constexpr float kTolerance = 1e-4f;
    constexpr float kMinRelFloor = 1e-6f;
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

    // scalar layout: must match GpuObjectUB in src/core/types.hpp
    struct ObjectRow
    {
        std::array<float, 16> modelMatrix; // column-major
        std::array<float, 16> prevModelMatrix;
        std::array<float, 4> boundingSphere;
        uint32_t modelIndex;
        uint32_t instanceFlags;
        uint32_t pad0;
        uint32_t pad1;
    };
    static_assert(sizeof(ObjectRow) == 160);

    // minimal frame root mirror: only instances @ offset 8 is read by toWorldLight
    struct FrameRootStub
    {
        VkDeviceAddress camera;
        VkDeviceAddress instances;
        std::array<uint8_t, 192 - 16> rest;
    };
    static_assert(sizeof(FrameRootStub) == 192);
    static_assert(offsetof(FrameRootStub, instances) == 8);

    // scalar layout: must match LightCase in tests/shaders/lights_numbers.slang
    struct LightCase
    {
        LightRow light;
        Vec3 p;
        uint32_t useWorldTransform;
    };
    static_assert(sizeof(LightCase) == 80);

    // scalar layout: must match LightPush in tests/shaders/lights_numbers.slang
    struct LightPush
    {
        VkDeviceAddress cases;
        VkDeviceAddress results;
        VkDeviceAddress frameRoot;
        uint32_t caseCount;
        uint32_t pad0;
    };
    static_assert(sizeof(LightPush) == 32);

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
        bool isCoincident = false; // d == 0: check finite radiance and zero/small distance, skip unit-l check
        bool checkWorldFields = false;
        Vec3 expectedWorldPos{};
        Vec3 expectedWorldDir{};
        float expectedRange = 0.0f;
        float expectedIntensity = 0.0f;
        float expectedInnerCone = 0.0f;
    };

    constexpr Vec3 kOrigin{0.0f, 0.0f, 0.0f};
    constexpr Vec3 kUp{0.0f, 0.0f, 1.0f};
    constexpr Vec3 kDown{0.0f, 0.0f, -1.0f};
    constexpr Vec3 kWhite{1.0f, 1.0f, 1.0f};

    double radians(double degrees) { return degrees * kPi / 180.0; }

    Vec3 scaled(Vec3 v, float s) { return {v.x * s, v.y * s, v.z * s}; }

    // column-major translation + scale matrix (optional 90-deg Y rotation)
    std::array<float, 16> makeTransform(Vec3 translate, Vec3 scale, bool rotateY90 = false)
    {
        if (!rotateY90) {
            return {
                scale.x, 0.0f, 0.0f, 0.0f,
                0.0f, scale.y, 0.0f, 0.0f,
                0.0f, 0.0f, scale.z, 0.0f,
                translate.x, translate.y, translate.z, 1.0f,
            };
        }
        // R_y(+90 deg) * S: +x -> -z, +z -> +x
        return {
            0.0f, 0.0f, -scale.x, 0.0f,
            0.0f, scale.y, 0.0f, 0.0f,
            scale.z, 0.0f, 0.0f, 0.0f,
            translate.x, translate.y, translate.z, 1.0f,
        };
    }

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
                       float innerCone = 0.0f, float outerCone = static_cast<float>(kPi / 4.0),
                       uint32_t instanceIndex = 0)
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
        light.instanceIndex = instanceIndex;
        return light;
    }

    TestCase directional(const char* name, Vec3 direction, Vec3 color, float intensity)
    {
        const LightRow light = makeLight(kDirectional, kOrigin, direction, color, intensity, 0.0f);
        return {name,
                {light, kOrigin, 0},
                scaled(color, intensity),
                {-direction.x, -direction.y, -direction.z},
                std::numeric_limits<float>::max()};
    }

    // light 2 m above the origin; p at polar angle degrees off the light's down axis, distance away
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
                {light, p, 0},
                scaled(light.color, static_cast<float>(light.intensity * attenuation)),
                l,
                static_cast<float>(distance)};
    }

    std::vector<ObjectRow> buildObjects()
    {
        std::vector<ObjectRow> objects(3);
        // 0: translated + non-uniformly scaled (3, 0.5, 2)
        objects[0].modelMatrix = makeTransform({10.0f, -4.0f, 2.0f}, {3.0f, 0.5f, 2.0f}, false);
        // 1: rotated +90 deg around Y with non-uniform scale (2, 4, 0.5)
        objects[1].modelMatrix = makeTransform({1.0f, 2.0f, 3.0f}, {2.0f, 4.0f, 0.5f}, true);
        // 2: mirrored (-2, 1, 1)
        objects[2].modelMatrix = makeTransform({0.0f, 0.0f, 0.0f}, {-2.0f, 1.0f, 1.0f}, false);
        return objects;
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

        // exact d == 0: light at the shading point stays finite, clamped to kMinDistanceSquared = 1e-4
        {
            LightRow atPoint = point;
            atPoint.position = {1.0f, 2.0f, 3.0f};
            TestCase tc{};
            tc.name = "point, exact d == 0";
            tc.input = {atPoint, {1.0f, 2.0f, 3.0f}, 0};
            tc.expectedRadiance = {40000.0f, 40000.0f, 40000.0f};
            tc.expectedDistance = 1e-6f;
            tc.isCoincident = true;
            cases.push_back(tc);
        }

        // toWorldLight: non-uniform scale must NOT scale intensity, range or cone, and must normalize direction
        {
            const float inner = static_cast<float>(radians(15.0));
            const float outer = static_cast<float>(radians(35.0));
            // model pos (1, 2, 1) under object 0 (T=(10,-4,2), S=(3,0.5,2)) -> world (13, -3, 4)
            // model dir (0, 0, -1) -> (0, 0, -2) normalized -> (0, 0, -1)
            const LightRow modelSpot =
                makeLight(kSpot, {1.0f, 2.0f, 1.0f}, kDown, kWhite, 8.0f, 6.0f, inner, outer, 0);
            const Vec3 p{13.0f, -3.0f, 2.0f}; // 2 m along world dir
            const double atten = khronosAttenuation(2.0, 6.0, 1.0, kSpot, inner, outer);
            TestCase tc{};
            tc.name = "toWorldLight, non-uniform scale keeps intensity/range/cone";
            tc.input = {modelSpot, p, 1};
            tc.expectedRadiance = scaled(kWhite, static_cast<float>(8.0 * atten));
            tc.expectedL = kUp;
            tc.expectedDistance = 2.0f;
            tc.checkWorldFields = true;
            tc.expectedWorldPos = {13.0f, -3.0f, 4.0f};
            tc.expectedWorldDir = kDown;
            tc.expectedRange = 6.0f;
            tc.expectedIntensity = 8.0f;
            tc.expectedInnerCone = inner;
            cases.push_back(tc);
        }
        {
            // object 1: R_y(+90) * S(2,4,0.5) + T(1,2,3); model dir (0, 0, -1) -> (-0.5, 0, 0) normalized -> (-1, 0, 0)
            // model pos (0, 0, 2) -> (1 + 1, 2, 3) = (2, 2, 3)
            const LightRow modelSpot = makeLight(kSpot, {0.0f, 0.0f, 2.0f}, kDown, kWhite, 4.0f, 0.0f, 0.0f,
                                                 static_cast<float>(kPi / 4.0), 1);
            const Vec3 p{0.0f, 2.0f, 3.0f}; // 2 m along (-1, 0, 0)
            TestCase tc{};
            tc.name = "toWorldLight, rotated + scaled spot";
            tc.input = {modelSpot, p, 1};
            tc.expectedRadiance = kWhite; // 4 / 2^2 = 1
            tc.expectedL = {1.0f, 0.0f, 0.0f};
            tc.expectedDistance = 2.0f;
            tc.checkWorldFields = true;
            tc.expectedWorldPos = {2.0f, 2.0f, 3.0f};
            tc.expectedWorldDir = {-1.0f, 0.0f, 0.0f};
            tc.expectedRange = 0.0f;
            tc.expectedIntensity = 4.0f;
            tc.expectedInnerCone = 0.0f;
            cases.push_back(tc);
        }
        {
            // object 2: mirrored (-2, 1, 1); model dir (1, 0, 0) -> (-2, 0, 0) normalized -> (-1, 0, 0)
            const LightRow modelDir = makeLight(kDirectional, kOrigin, {1.0f, 0.0f, 0.0f}, kWhite, 5.0f, 0.0f, 0.0f,
                                                static_cast<float>(kPi / 4.0), 2);
            TestCase tc{};
            tc.name = "toWorldLight, mirrored directional";
            tc.input = {modelDir, kOrigin, 1};
            tc.expectedRadiance = {5.0f, 5.0f, 5.0f};
            tc.expectedL = {1.0f, 0.0f, 0.0f};
            tc.expectedDistance = std::numeric_limits<float>::max();
            tc.checkWorldFields = true;
            tc.expectedWorldPos = kOrigin;
            tc.expectedWorldDir = {-1.0f, 0.0f, 0.0f};
            tc.expectedRange = 0.0f;
            tc.expectedIntensity = 5.0f;
            tc.expectedInnerCone = 0.0f;
            cases.push_back(tc);
        }
        return cases;
    }

    bool isClose(float actual, float expected)
    {
        if (expected == 0.0f) {
            return actual == 0.0f;
        }
        return std::fabs(actual - expected) <= kTolerance * std::fmax(std::fabs(expected), kMinRelFloor);
    }

    bool isCloseVec(const float* actual, Vec3 expected)
    {
        const auto closeComp = [](float a, float e)
        { return std::fabs(a - e) <= kTolerance * std::fmax(std::fabs(e), 1e-3f); };
        return closeComp(actual[0], expected.x) && closeComp(actual[1], expected.y) && closeComp(actual[2], expected.z);
    }

    bool isCloseRadiance(const float* actual, Vec3 expected)
    {
        return isClose(actual[0], expected.x) && isClose(actual[1], expected.y) && isClose(actual[2], expected.z);
    }

    void verify(const TestCase& testCase, const float* radiance, const float* lAndCone, const float* worldPosRange,
                const float* worldDirIntensity)
    {
        std::printf("  %s: radiance (%.5f %.5f %.5f) distance %g, l (%.4f %.4f %.4f)\n", testCase.name, radiance[0],
                    radiance[1], radiance[2], radiance[3], lAndCone[0], lAndCone[1], lAndCone[2]);
        check(std::isfinite(radiance[0]) && std::isfinite(radiance[1]) && std::isfinite(radiance[2]) &&
                  std::isfinite(radiance[3]) && std::isfinite(lAndCone[0]) && std::isfinite(lAndCone[1]) &&
                  std::isfinite(lAndCone[2]),
              "  outputs are finite");
        check(isCloseRadiance(radiance, testCase.expectedRadiance), "  radiance matches KHR_lights_punctual");
        if (!testCase.isCoincident) {
            check(isCloseVec(lAndCone, testCase.expectedL), "  l points toward the light");
        }
        check(radiance[3] == testCase.expectedDistance || isClose(radiance[3], testCase.expectedDistance),
              "  distance is the shadow ray tMax");
        if (testCase.checkWorldFields) {
            check(isCloseVec(worldPosRange, testCase.expectedWorldPos), "  world position matches transform");
            check(isCloseVec(worldDirIntensity, testCase.expectedWorldDir), "  world direction is unit transformed");
            check(isClose(worldPosRange[3], testCase.expectedRange), "  range is not scaled");
            check(isClose(worldDirIntensity[3], testCase.expectedIntensity), "  intensity is not scaled");
            check(isClose(lAndCone[3], testCase.expectedInnerCone), "  cone angle is not scaled");
        }
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
        const std::vector<ObjectRow> objects = buildObjects();
        const auto caseCount = static_cast<uint32_t>(cases.size());
        test::HostBuffer inputs = test::createHostBuffer(vulkan, caseCount * sizeof(LightCase));
        test::HostBuffer results = test::createHostBuffer(vulkan, caseCount * kResultRows * 4 * sizeof(float));
        test::HostBuffer objectBuf = test::createHostBuffer(vulkan, objects.size() * sizeof(ObjectRow));
        test::HostBuffer rootBuf = test::createHostBuffer(vulkan, sizeof(FrameRootStub));

        auto* inputData = static_cast<LightCase*>(inputs.mapped);
        for (uint32_t i = 0; i < caseCount; ++i) {
            inputData[i] = cases[i].input;
        }
        auto* objectData = static_cast<ObjectRow*>(objectBuf.mapped);
        for (size_t i = 0; i < objects.size(); ++i) {
            objectData[i] = objects[i];
        }
        auto* rootData = static_cast<FrameRootStub*>(rootBuf.mapped);
        rootData->instances = objectBuf.address;

        const LightPush push{inputs.address, results.address, rootBuf.address, caseCount, 0};
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
            verify(cases[i], row, row + 4, row + 8, row + 12);
        }
        test::destroyHostBuffer(vulkan, inputs);
        test::destroyHostBuffer(vulkan, results);
        test::destroyHostBuffer(vulkan, objectBuf);
        test::destroyHostBuffer(vulkan, rootBuf);
        vkDestroyPipeline(vulkan.device, pipeline, nullptr);
    }

    vkDestroyPipelineLayout(vulkan.device, layout, nullptr);
    return test::finish();
}
