// Round-trip test for SHaRC on the engine's slangc flags and BDA binding model:
// one update pass writes two-hit paths, resolve merges them, query reads them back.
// Fails if a SHaRC bump or a Slang upgrade breaks codegen (64-bit atomics, fp16, pointers).
#include "test_support.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using test::check;

namespace
{
    // power of two, as SHaRC recommends
    constexpr uint32_t kCapacity = 4096;
    constexpr uint32_t kProbeCount = 64;
    constexpr uint32_t kGroupSize = 64;
    constexpr uint32_t kQueryCount = kProbeCount * 2;
    constexpr float kSecondHitThroughput = 0.5f;
    // fp16 resolved storage and 1e3 radiance quantization
    constexpr float kTolerance = 1e-2f;

    // sizes from SHaRC docs/Integration.md (default layout, 64-bit keys)
    constexpr VkDeviceSize kHashEntryStride = 8;
    constexpr VkDeviceSize kAccumulationStride = 16;
    constexpr VkDeviceSize kResolvedStride = 16;

    // scalar layout: must match SharcPush in shaders/sharc_roundtrip.slang
    struct SharcPush
    {
        VkDeviceAddress hashEntries;
        VkDeviceAddress accumulation;
        VkDeviceAddress resolved;
        VkDeviceAddress results;
        float cameraPosition[3];
        uint32_t capacity;
        uint32_t probeCount;
    };
    static_assert(offsetof(SharcPush, cameraPosition) == 32);
    static_assert(offsetof(SharcPush, capacity) == 44);
    static_assert(offsetof(SharcPush, probeCount) == 48);

    struct Float3
    {
        float x, y, z;
    };

    // mirrors directLighting() in the shader
    Float3 directLighting(uint32_t probe, uint32_t hit)
    {
        return {static_cast<float>(probe + 1) * 0.1f, static_cast<float>(probe % 5 + 1) * 0.1f,
                static_cast<float>(hit + 1) * 0.1f};
    }

    struct HostBuffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceAddress address = 0;
    };

    // host-visible so the test can zero and read back without staging
    HostBuffer createBuffer(const test::VulkanContext& vulkan, VkDeviceSize size)
    {
        HostBuffer result;
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        vkCreateBuffer(vulkan.device, &bufferInfo, nullptr, &result.buffer);

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(vulkan.device, result.buffer, &requirements);
        VkPhysicalDeviceMemoryProperties memoryProps{};
        vkGetPhysicalDeviceMemoryProperties(vulkan.gpu, &memoryProps);
        constexpr VkMemoryPropertyFlags kHostFlags =
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        uint32_t memoryType = UINT32_MAX;
        for (uint32_t i = 0; i < memoryProps.memoryTypeCount; ++i) {
            const VkMemoryPropertyFlags flags = memoryProps.memoryTypes[i].propertyFlags;
            if ((requirements.memoryTypeBits & (1u << i)) && (flags & kHostFlags) == kHostFlags) {
                memoryType = i;
                if (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
                    break;
                }
            }
        }

        VkMemoryAllocateFlagsInfo flagsInfo{};
        flagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        flagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.pNext = &flagsInfo;
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex = memoryType;
        vkAllocateMemory(vulkan.device, &allocInfo, nullptr, &result.memory);
        vkBindBufferMemory(vulkan.device, result.buffer, result.memory, 0);
        vkMapMemory(vulkan.device, result.memory, 0, VK_WHOLE_SIZE, 0, &result.mapped);
        // SHaRC requires zero-initialized buffers
        std::memset(result.mapped, 0, size);

        VkBufferDeviceAddressInfo addressInfo{};
        addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        addressInfo.buffer = result.buffer;
        result.address = vkGetBufferDeviceAddress(vulkan.device, &addressInfo);
        return result;
    }

    void destroyBuffer(const test::VulkanContext& vulkan, HostBuffer& buffer)
    {
        vkDestroyBuffer(vulkan.device, buffer.buffer, nullptr);
        vkFreeMemory(vulkan.device, buffer.memory, nullptr);
    }

    std::vector<uint32_t> readSpirv(const std::string& path)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            return {};
        }
        std::vector<uint32_t> code(static_cast<size_t>(file.tellg()) / sizeof(uint32_t));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(code.size() * sizeof(uint32_t)));
        return code;
    }

    VkPipeline createPipeline(const test::VulkanContext& vulkan, VkPipelineLayout layout, const char* entry)
    {
        const std::string path = std::string(SHARC_TEST_SHADER_DIR) + "/" + entry + ".spv";
        const std::vector<uint32_t> code = readSpirv(path);
        if (code.empty()) {
            std::printf("  missing %s\n", path.c_str());
            return VK_NULL_HANDLE;
        }

        VkShaderModuleCreateInfo moduleInfo{};
        moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        moduleInfo.codeSize = code.size() * sizeof(uint32_t);
        moduleInfo.pCode = code.data();
        VkShaderModule module = VK_NULL_HANDLE;
        vkCreateShaderModule(vulkan.device, &moduleInfo, nullptr, &module);

        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = module;
        pipelineInfo.stage.pName = entry;
        pipelineInfo.layout = layout;
        VkPipeline pipeline = VK_NULL_HANDLE;
        const VkResult result =
            vkCreateComputePipelines(vulkan.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
        vkDestroyShaderModule(vulkan.device, module, nullptr);
        return result == VK_SUCCESS ? pipeline : VK_NULL_HANDLE;
    }

    void barrier(VkCommandBuffer cmd, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess)
    {
        VkMemoryBarrier memoryBarrier{};
        memoryBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        memoryBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        memoryBarrier.dstAccessMask = dstAccess;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, dstStage, 0, 1, &memoryBarrier, 0, nullptr, 0,
                             nullptr);
    }

    bool isNear(float actual, float expected)
    {
        return std::fabs(actual - expected) <= kTolerance * std::fmax(1.0f, std::fabs(expected));
    }

    void verifyResults(const float* results)
    {
        uint32_t cachedCount = 0;
        uint32_t matchCount = 0;
        for (uint32_t query = 0; query < kQueryCount; ++query) {
            const bool isSecondHit = query >= kProbeCount;
            const uint32_t probe = isSecondHit ? query - kProbeCount : query;
            const Float3 own = directLighting(probe, isSecondHit ? 1 : 0);
            const Float3 next = directLighting(probe, 1);
            // first hit = own direct light + throughput * second hit radiance
            const Float3 expected = isSecondHit ? own
                                                : Float3{own.x + kSecondHitThroughput * next.x,
                                                         own.y + kSecondHitThroughput * next.y,
                                                         own.z + kSecondHitThroughput * next.z};
            const float* actual = results + static_cast<size_t>(query) * 4;
            const bool isCached = actual[3] == 1.0f;
            const bool isMatch = isNear(actual[0], expected.x) && isNear(actual[1], expected.y) &&
                                 isNear(actual[2], expected.z);
            cachedCount += isCached ? 1 : 0;
            matchCount += isCached && isMatch ? 1 : 0;
            if (!isCached || !isMatch) {
                std::printf("  query %u: cached %d got (%.4f %.4f %.4f) want (%.4f %.4f %.4f)\n", query, isCached,
                            actual[0], actual[1], actual[2], expected.x, expected.y, expected.z);
            }
        }
        check(cachedCount == kQueryCount, "every probe hits the cache");
        check(matchCount == kQueryCount, "cached radiance matches direct + propagated light");
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

    VkPushConstantRange pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SharcPush)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    vkCreatePipelineLayout(vulkan.device, &layoutInfo, nullptr, &layout);

    const std::array<VkPipeline, 3> pipelines = {createPipeline(vulkan, layout, "updateMain"),
                                                 createPipeline(vulkan, layout, "resolveMain"),
                                                 createPipeline(vulkan, layout, "queryMain")};
    const bool hasPipelines = check(pipelines[0] != VK_NULL_HANDLE && pipelines[1] != VK_NULL_HANDLE &&
                                        pipelines[2] != VK_NULL_HANDLE,
                                    "update / resolve / query pipelines created");

    HostBuffer hashEntries = createBuffer(vulkan, kCapacity * kHashEntryStride);
    HostBuffer accumulation = createBuffer(vulkan, kCapacity * kAccumulationStride);
    HostBuffer resolved = createBuffer(vulkan, kCapacity * kResolvedStride);
    HostBuffer results = createBuffer(vulkan, kQueryCount * 4 * sizeof(float));

    if (hasPipelines) {
        const SharcPush push{hashEntries.address, accumulation.address, resolved.address, results.address,
                             {3.5f, 3.5f, -10.0f}, kCapacity, kProbeCount};
        const std::array<uint32_t, 3> groupCounts = {(kProbeCount + kGroupSize - 1) / kGroupSize,
                                                     kCapacity / kGroupSize,
                                                     (kQueryCount + kGroupSize - 1) / kGroupSize};

        VkCommandBuffer cmd = vulkan.beginCommands();
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        for (size_t pass = 0; pass < pipelines.size(); ++pass) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[pass]);
            vkCmdDispatch(cmd, groupCounts[pass], 1, 1);
            barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        }
        barrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
        check(vulkan.submit(cmd), "update -> resolve -> query submitted");
        vulkan.waitIdle();

        const auto* keys = static_cast<const uint64_t*>(hashEntries.mapped);
        uint32_t entryCount = 0;
        for (uint32_t i = 0; i < kCapacity; ++i) {
            entryCount += keys[i] != 0 ? 1 : 0;
        }
        std::printf("  hash entries used %u\n", entryCount);
        check(entryCount == kQueryCount, "one hash entry per distinct hit");
        verifyResults(static_cast<const float*>(results.mapped));
    }

    for (HostBuffer* buffer : {&hashEntries, &accumulation, &resolved, &results}) {
        destroyBuffer(vulkan, *buffer);
    }
    for (VkPipeline pipeline : pipelines) {
        vkDestroyPipeline(vulkan.device, pipeline, nullptr);
    }
    vkDestroyPipelineLayout(vulkan.device, layout, nullptr);
    return test::finish();
}
