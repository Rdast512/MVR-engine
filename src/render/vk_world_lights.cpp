#include "vk_world_lights.hpp"
#include "../static_headers/logger.hpp"
#include "../util/vk_tracy.hpp"
#include "../util/vk_utils.hpp"
#include "push_data.hpp"
#include "scene/vk_scene.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string_view>
#include <unordered_map>

#include <glm/gtc/constants.hpp>
#include <glm/trigonometric.hpp>

namespace
{
    // matches passes/light_world.slang
    constexpr uint32_t kLightWorldGroupSize = 64;

    constexpr uint32_t divideRoundUp(uint32_t value, uint32_t divisor) { return (value + divisor - 1) / divisor; }
} // namespace

WorldLights::Sun WorldLights::defaultSun()
{
    return Sun{
        .isEnabled = true,
        // along Sponza's long courtyard: a sun across it is blocked by the opposite wing up to ~55 degrees
        .azimuthDegrees = 20.0f,
        .elevationDegrees = 65.0f,
        // FP16 radiance without pre-exposure (considerations C-H2-1); auto exposure makes the scale irrelevant
        .illuminance = 10.0f,
        .color = glm::vec3(1.0f),
    };
}

uint32_t WorldLights::lightCount() const
{
    return assets.uploadedCount(AssetBuffer::Lights) + triangleLightCount() + (sun.isEnabled ? 1u : 0u);
}

uint32_t WorldLights::localLightCount() const
{
    return punctualLocalLightCount() + (areTriangleLightsSampled ? triangleLightCount() : 0u);
}

uint32_t WorldLights::punctualLocalLightCount() const { return assets.localLightCount(); }

uint32_t WorldLights::triangleLightCount() const { return rtScene.triangleLightCount(); }

uint32_t WorldLights::firstInfiniteLight() const { return punctualLocalLightCount() + triangleLightCount(); }

uint32_t WorldLights::infiniteLightCount() const { return lightCount() - firstInfiniteLight(); }

WorldLights::WorldLights(const Device& device, const VkAllocator& allocator, const FrameSync& frameSync,
                         const AssetBuffers& assets, const FrameData& frameData, const RayTracingScene& rtScene,
                         const ShaderObjects& shaderObjects) :
    device(device), allocator(allocator), frameSync(frameSync), assets(assets), frameData(frameData),
    rtScene(rtScene), shaderObjects(shaderObjects)
{
}

WorldLights::~WorldLights()
{
    for (SlotBuffer& slot : slots) {
        destroySlot(slot);
    }
}

void WorldLights::destroySlot(SlotBuffer& slot) const
{
    destroyVmaBuffer(allocator.allocator, slot.buffer, slot.memory);
    slot.address = 0;
    slot.capacity = 0;
}

void WorldLights::prepareFrame(uint32_t frameSlot, uint64_t lastSubmittedFrame)
{
    const uint32_t count = lightCount();
    SlotBuffer& slot = slots[frameSlot];
    if (count <= slot.capacity) {
        return;
    }
    ZoneScopedN("WorldLights::grow");
    // rare: only a light count above every earlier one
    if (lastSubmittedFrame > 0 && slot.memory != nullptr) {
        const vk::SemaphoreWaitInfo waitInfo{
            .semaphoreCount = 1, .pSemaphores = &*frameSync.rayTimeline, .pValues = &lastSubmittedFrame};
        while (vk::Result::eTimeout == device.vkdevice.waitSemaphores(waitInfo, UINT64_MAX))
            ;
    }
    destroySlot(slot);
    // powers of two: a scene that loads lights one model at a time reallocates rarely
    slot.capacity = std::bit_ceil(count);
    createBuffer(slot.capacity * sizeof(GpuLight),
                 vk::BufferUsageFlagBits2::eStorageBuffer | vk::BufferUsageFlagBits2::eShaderDeviceAddress |
                     vk::BufferUsageFlagBits2::eTransferSrc,
                 vk::MemoryPropertyFlagBits::eDeviceLocal, slot.buffer, slot.memory, allocator.allocator,
                 device.vkdevice, device.queueFamilyIndices, std::format("WorldLights_{}", frameSlot));
    slot.address = device.vkdevice.getBufferAddress({.buffer = *slot.buffer});
}

void WorldLights::recordUpdate(const vk::raii::CommandBuffer& cmd, uint32_t frameSlot,
                               const LightPdfTarget& lightPdf) const
{
    // the frame root carries the same count; the PDF's texels past it are cleared by the same threads
    const uint32_t count = std::max(lightCount(), lightPdf.texelCount);
    if (count == 0) {
        return;
    }
    const float azimuth = glm::radians(sun.azimuthDegrees);
    const float elevation = glm::radians(sun.elevationDegrees);
    const glm::vec3 towardSun{std::cos(elevation) * std::cos(azimuth), std::sin(elevation),
                              std::cos(elevation) * std::sin(azimuth)};
    // readers wait for the compute timeline, which covers these writes. The slot's earlier readers are done: its own
    // frame by the slot fence, the next frame (previous-frame rows) by this submit's ray timeline wait
    const LightWorldPushData push{
        .frameRoot = frameData.frameRootAddress(frameSlot),
        .sunDirection = -towardSun,
        .sunIlluminance = sun.illuminance,
        .sunColor = sun.color,
        .assetLightCount = assets.uploadedCount(AssetBuffer::Lights),
        .triangleLightRefs = rtScene.triangleLightRefsAddress(frameSlot),
        .triangleLightRefCount = rtScene.triangleLightCount(),
        .lightPdf = lightPdf.mip0,
        .lightPdfTexelCount = lightPdf.texelCount,
        .pad0 = 0,
    };
    ShaderObjects::bindComputeShader(cmd, shaderObjects.frameShaders.lightWorld);
    cmd.pushDataEXT(vk::PushDataInfoEXT{.offset = 0, .data = {.address = &push, .size = sizeof(push)}});
    cmd.dispatch(divideRoundUp(count, kLightWorldGroupSize), 1, 1);
}

bool WorldLights::isPdfStatsRequested()
{
    const char* text = std::getenv("ENGINE_LIGHT_PDF_STATS");
    return text != nullptr && std::string_view(text) == "1";
}

void WorldLights::dumpLightPdfStats(uint32_t frameSlot, FrameSync& sync, const Scene& scene,
                                    const glm::mat4& viewProj) const
{
    const uint32_t localCount = localLightCount();
    const SlotBuffer& slot = slots[frameSlot];
    if (localCount == 0 || slot.memory == nullptr) {
        logInfo("Light PDF stats: 0 local lights", "LightPdfStats");
        return;
    }
    const vk::DeviceSize bytes = static_cast<vk::DeviceSize>(localCount) * sizeof(GpuLight);
    vk::raii::Buffer staging = nullptr;
    VmaAllocation stagingMem = nullptr;
    createBuffer(bytes, vk::BufferUsageFlagBits2::eTransferDst,
                 vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent |
                     vk::MemoryPropertyFlagBits::eHostCached,
                 staging, stagingMem, allocator.allocator, device.vkdevice, device.queueFamilyIndices,
                 "LightPdfStatsStaging", VMA_ALLOCATION_CREATE_MAPPED_BIT);
    {
        const FrameSync::OneTimeCommands commands = sync.computeCommands();
        commands.cmd.begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
        commands.cmd.copyBuffer(*slot.buffer, *staging, vk::BufferCopy{.srcOffset = 0, .dstOffset = 0, .size = bytes});
        submitAndWait(commands.cmd, commands.queue);
    }
    VmaAllocationInfo allocInfo{};
    vmaGetAllocationInfo(allocator.allocator, stagingMem, &allocInfo);
    const auto* lights = static_cast<const GpuLight*>(allocInfo.pMappedData);

    const auto lum = [](glm::vec3 c) { return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b; };
    const uint32_t firstTri = punctualLocalLightCount();
    const std::span<const GpuTriangleLightRef> refs = rtScene.getTriangleLightRefs();
    const std::span<const GpuRayInstance> rayInstances = rtScene.getRayInstances();

    struct MatStat
    {
        uint32_t totalTris = 0;
        uint32_t zeroTris = 0;
        uint32_t cafeTris = 0;
        double power = 0.0;
        double cafePower = 0.0;
    };
    std::unordered_map<uint32_t, MatStat> byMaterial;

    double totalPower = 0.0;
    double cafePower = 0.0;
    double frustumPower = 0.0;
    uint32_t totalTris = 0;
    uint32_t zeroTris = 0;

    for (uint32_t i = 0; i < firstTri && i < localCount; ++i) {
        const double p = 4.0 * glm::pi<double>() * lum(lights[i].color * lights[i].intensity);
        totalPower += p;
    }

    for (size_t t = 0; t < refs.size(); ++t) {
        const uint32_t row = firstTri + static_cast<uint32_t>(t);
        if (row >= localCount) {
            break;
        }
        const uint32_t primRow = rayInstances[refs[t].rayInstance].primitiveRow;
        const uint32_t matId = scene.geometryStore.primitiveDraws[primRow].materialId;
        MatStat& stat = byMaterial[matId];
        GpuTriangleLight tri{};
        std::memcpy(&tri, &lights[row], sizeof(tri));
        const glm::vec3 crossE = glm::cross(tri.edge1, tri.edge2);
        const double area = 0.5 * static_cast<double>(glm::length(crossE));
        const double faces = (tri.flags & GpuTriangleLightFlag::DoubleSided) != 0 ? 2.0 : 1.0;
        const double power = area * glm::pi<double>() * faces * lum(tri.radiance);
        const glm::vec3 centroid = tri.v0 + (tri.edge1 + tri.edge2) / 3.0f;
        const bool inCafe = centroid.x >= -12.3f && centroid.x <= 1.5f && centroid.y >= 0.0f && centroid.y <= 4.5f &&
            centroid.z >= 0.9f && centroid.z <= 16.1f;
        const glm::vec4 clip = viewProj * glm::vec4(centroid, 1.0f);
        const bool inFrustum = clip.w > 0.0f && std::abs(clip.x) <= clip.w && std::abs(clip.y) <= clip.w &&
            clip.z >= 0.0f && clip.z <= clip.w;
        ++totalTris;
        ++stat.totalTris;
        if (power <= 0.0) {
            ++zeroTris;
            ++stat.zeroTris;
        }
        if (inCafe) {
            ++stat.cafeTris;
            stat.cafePower += power;
            cafePower += power;
        }
        if (inFrustum) {
            frustumPower += power;
        }
        stat.power += power;
        totalPower += power;
    }

    logInfo(std::format("Light PDF stats: {} local lights ({} punctual, {} tris), zero-power tris {} ({:.1f}%), total "
                        "power {:.3f}, cafe share {:.2f}%, frustum share {:.2f}%",
                        localCount, firstTri, totalTris, zeroTris, totalTris > 0 ? 100.0 * zeroTris / totalTris : 0.0,
                        totalPower, totalPower > 0.0 ? 100.0 * cafePower / totalPower : 0.0,
                        totalPower > 0.0 ? 100.0 * frustumPower / totalPower : 0.0),
            "LightPdfStats");

    std::vector<std::pair<uint32_t, MatStat>> sorted(byMaterial.begin(), byMaterial.end());
    std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.second.power > b.second.power; });
    const size_t topN = std::min<size_t>(sorted.size(), 10);
    for (size_t rank = 0; rank < topN; ++rank) {
        const auto& [matId, st] = sorted[rank];
        const std::string_view name =
            matId < scene.materialStore.infos.size() ? std::string_view(scene.materialStore.infos[matId].name) : "?";
        logInfo(std::format("  #{:2d} mat {:3d} ({}): power {:.3f} ({:5.2f}%), tris {:5d} (zero {:5d}, cafe {:5d}, "
                            "cafe power {:5.2f}%)",
                            rank + 1, matId, name, st.power, totalPower > 0.0 ? 100.0 * st.power / totalPower : 0.0,
                            st.totalTris, st.zeroTris, st.cafeTris,
                            totalPower > 0.0 ? 100.0 * st.cafePower / totalPower : 0.0),
                "LightPdfStats");
    }
    destroyVmaBuffer(allocator.allocator, staging, stagingMem);
}
