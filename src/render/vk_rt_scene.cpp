#include "vk_rt_scene.hpp"
#include "../core/material_store.hpp"
#include "../util/debug.hpp"
#include "../util/vk_tracy.hpp"
#include "push_data.hpp"
#include "static_headers/logger.hpp"

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <format>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace
{
    constexpr const char* kRayInstancesTracyName = "GPU/RayInstances";
    constexpr const char* kRayBuildsTracyName = "GPU/RayInstanceBuilds";
    constexpr const char* kRayInstanceLightsTracyName = "GPU/RayInstanceLights";
    constexpr const char* kTriangleLightTasksTracyName = "GPU/TriangleLightTasks";
    constexpr const char* kPtlasOperationTracyName = "GPU/PtlasOperation";
    constexpr const char* kPtlasArgCountReadbackTracyName = "GPU/PtlasArgCountReadback";
    // first allocation, in instances
    constexpr uint32_t kMinCapacity = 64;
    // matches shaders/passes/rt_instances.slang
    constexpr uint32_t kRtInstancesGroupSize = 64;
    // PtlasInstanceState in rt_instances.slang: the 3x4 transform the PTLAS holds
    constexpr vk::DeviceSize kPtlasStateRowBytes = 48;
    // VkPartitionedAccelerationStructureWriteInstanceDataNV::instanceID carries the primitive row
    constexpr uint32_t kCustomIndexLimit = 1u << 24;
    // VUID-vkCmdBuildPartitionedAccelerationStructuresNV-pBuildInfo-10542/-10544/-10545: 256-byte aligned
    // scratch, source and destination
    constexpr vk::DeviceSize kPtlasAlignment = 256;
    // srcInfos / srcInfosCount of the build: one write-instance op, then its count
    constexpr vk::DeviceSize kOpCountOffset = sizeof(vk::BuildPartitionedAccelerationStructureIndirectCommandNV);
    constexpr vk::DeviceSize kOperationBytes = kOpCountOffset + sizeof(uint32_t);
    constexpr vk::DeviceSize kArgCountOffset =
        offsetof(VkBuildPartitionedAccelerationStructureIndirectCommandNV, argCount);
    // MG-B4: geometric growth margin when a moving entity exceeds sceneBounds (avoids per-frame NRC reconfigures)
    constexpr float kDynamicBoundsGrowthFraction = 0.25f;
    constexpr float kDynamicBoundsMinMargin = 1.0f;
    // MG-P5: derived-grid refinement limits (vk_lod_clusters scene.hpp:349-355)
    constexpr uint32_t kDerivedMaxInstances = 1024;
    constexpr uint32_t kDerivedMaxPartitions = 4096;
    constexpr uint32_t kMaxGridAxis = 128;

    // interleave low 16 bits for 2D Morton Z-order (vk_lod_clusters scene_partitions.cpp:17)
    [[nodiscard]] constexpr uint32_t part1By1(uint32_t v) noexcept
    {
        v &= 0x0000ffffu;
        v = (v ^ (v << 8)) & 0x00ff00ffu;
        v = (v ^ (v << 4)) & 0x0f0f0f0fu;
        v = (v ^ (v << 2)) & 0x33333333u;
        v = (v ^ (v << 1)) & 0x55555555u;
        return v;
    }

    template <std::unsigned_integral T>
    constexpr T divideRoundUp(T value, T divisor)
    {
        return (value + divisor - 1) / divisor;
    }

    // transforms an AABB's 8 corners by matrix and merges them into target
    void expandBounds(BoundingBox& target, const glm::mat4& matrix, const BoundingBox& box)
    {
        for (uint32_t corner = 0; corner < 8; ++corner) {
            const glm::vec3 local{(corner & 1u) != 0 ? box.bboxMax.x : box.bboxMin.x,
                                  (corner & 2u) != 0 ? box.bboxMax.y : box.bboxMin.y,
                                  (corner & 4u) != 0 ? box.bboxMax.z : box.bboxMin.z};
            const glm::vec3 position = glm::vec3(matrix * glm::vec4(local, 1.0f));
            target.bboxMin = glm::min(target.bboxMin, position);
            target.bboxMax = glm::max(target.bboxMax, position);
        }
    }

    // Morton Z-order index in [0, axis * axis) for power-of-two axis
    [[nodiscard]] uint32_t mortonCellForCenter(glm::vec2 center, glm::vec2 lower, glm::vec2 extent, uint32_t axis)
    {
        const glm::vec2 normalized = (center - lower) / extent * static_cast<float>(axis);
        const glm::ivec2 cell = glm::clamp(glm::ivec2(glm::floor(normalized)), glm::ivec2(0),
                                           glm::ivec2(static_cast<int32_t>(axis - 1)));
        return part1By1(static_cast<uint32_t>(cell.x)) | (part1By1(static_cast<uint32_t>(cell.y)) << 1);
    }

    // mesh-space box of a primitive's meshlet groups
    [[nodiscard]] BoundingBox primitiveBounds(const GeometryStore& geometry, const PrimitiveDraw& primitive)
    {
        BoundingBox box{.bboxMin = glm::vec3(std::numeric_limits<float>::max()),
                        .bboxMax = glm::vec3(std::numeric_limits<float>::lowest())};
        for (uint32_t g = primitive.firstGroup; g < primitive.firstGroup + primitive.groupCount; ++g) {
            box.bboxMin = glm::min(box.bboxMin, geometry.meshletGroups[g].bboxMin);
            box.bboxMax = glm::max(box.bboxMax, geometry.meshletGroups[g].bboxMax);
        }
        return box;
    }
} // namespace

RayTracingScene::RayTracingScene(const Device& device, const VkAllocator& allocator, const FrameSync& frameSync,
                                 const Scene& scene, const AssetBuffers& assets, const FrameData& frameData,
                                 const ShaderObjects& shaderObjects) :
    device(device), allocator(allocator), frameSync(frameSync), scene(scene), assets(assets), frameData(frameData),
    shaderObjects(shaderObjects)
{
    static_assert(sizeof(vk::PartitionedAccelerationStructureWriteInstanceDataNV) == 104);
    static_assert(sizeof(vk::BuildPartitionedAccelerationStructureIndirectCommandNV) == 24);
    if (const char* reassignEnv = std::getenv("ENGINE_PTLAS_REASSIGN"); reassignEnv != nullptr) {
        settings.isPartitionReassignmentEnabled = std::string_view(reassignEnv) == "1";
    }
    if (const char* fullEnv = std::getenv("ENGINE_PTLAS_FULL_BUILD"); fullEnv != nullptr) {
        settings.isFullBuildForced = std::string_view(fullEnv) == "1";
    }
    const vk::PhysicalDeviceLimits& limits = device.capabilities.properties2.properties.limits;
    const auto queueFamilies = device.physicalDevice.getQueueFamilyProperties2();
    if (limits.timestampPeriod > 0.0f && queueFamilies[device.computeIndex].queueFamilyProperties.timestampValidBits != 0) {
        timestampPeriodNs = limits.timestampPeriod;
        timestamps = vk::raii::QueryPool(device.vkdevice, vk::QueryPoolCreateInfo{
                                                              .queryType = vk::QueryType::eTimestamp,
                                                              .queryCount = kQueriesPerFrame * MAX_FRAMES_IN_FLIGHT,
                                                          });
    }
}

RayTracingScene::~RayTracingScene()
{
    for (FrameBuffers& buffers : frames) {
        destroyFrameBuffers(buffers);
    }
    destroyPtlas();
}

void RayTracingScene::prepareFrame(uint32_t frameSlot, uint64_t lastSubmittedFrame)
{
    ZoneScopedN("RayTracingScene::prepareFrame");
    collectResults(frameSlot);
    isSceneChangedThisFrame = false;
    if (templates.layoutGeneration != frameData.layoutGeneration() ||
        templates.instancesPerPartition != settings.instancesPerPartition ||
        templates.isPartitionReassignmentEnabled != settings.isPartitionReassignmentEnabled) {
        if (templates.layoutGeneration != frameData.layoutGeneration()) {
            isSceneChangedThisFrame = true;
        }
        buildTemplates();
    } else {
        updateDynamicEntities();
    }
    const PtlasShape& shape = templates.shape;
    const PtlasShape& sized = ptlas.sizedFor;
    if (ptlas.storage.memory == nullptr || shape.instances > sized.instances || shape.partitions > sized.partitions ||
        shape.maxPerPartition > sized.maxPerPartition) {
        createPtlas(lastSubmittedFrame);
    }
    if (frames[frameSlot].version != templates.version) {
        uploadTemplates(frameSlot);
    } else if (frames[frameSlot].buildsVersion != templates.buildsVersion) {
        std::memcpy(frames[frameSlot].builds.mapped, templates.builds.data(), std::span(templates.builds).size_bytes());
        frames[frameSlot].buildsVersion = templates.buildsVersion;
    }
    // the GPU appends to argCount; the slot's last build has completed
    auto* operation = static_cast<std::byte*>(frames[frameSlot].operation.mapped);
    const vk::BuildPartitionedAccelerationStructureIndirectCommandNV writeOp{
        .opType = vk::PartitionedAccelerationStructureOpTypeNV::eWriteInstance,
        .argCount = 0,
        .argData = {.startAddress = frames[frameSlot].writeInfos.address,
                    .strideInBytes = sizeof(vk::PartitionedAccelerationStructureWriteInstanceDataNV)},
    };
    constexpr uint32_t kOpCount = 1;
    std::memcpy(operation, &writeOp, sizeof(writeOp));
    std::memcpy(operation + kOpCountOffset, &kOpCount, sizeof(kOpCount));
    *static_cast<uint32_t*>(frames[frameSlot].argCountReadback.mapped) = 0;
}

// same entity filter as the cull templates (MeshletCulling::buildTemplates): one row per drawn primitive. Each row's
// partition is its cell in a uniform XZ grid over the primitives' world-space centers at layout time
void RayTracingScene::buildTemplates()
{
    ZoneScopedN("RayTracingScene::buildTemplates");
    templates.rows.clear();
    templates.builds.clear();
    templates.rowLocalCenters.clear();
    templates.firstLights.clear();
    templates.lightTasks.clear();
    templates.dynamicEntities.clear();
    templates.partitionOccupancy.clear();
    templates.cellToPartition.clear();
    templates.triangleLightCount = 0;
    templates.layoutGeneration = frameData.layoutGeneration();
    templates.instancesPerPartition = settings.instancesPerPartition;
    templates.isPartitionReassignmentEnabled = settings.isPartitionReassignmentEnabled;
    ++templates.version;
    templates.buildsVersion = templates.version;
    templates.sceneBounds = BoundingBox{.bboxMin = glm::vec3(std::numeric_limits<float>::max()),
                                        .bboxMax = glm::vec3(std::numeric_limits<float>::lowest())};

    const ObjectStorage& storage = scene.objectStorage;
    const GeometryStore& geometry = scene.geometryStore;
    const MaterialStore& materials = scene.materialStore;
    // the GPU table holds packed rows; kNoneIndex is the default material, which does not emit
    const auto isEmissive = [&materials](uint32_t materialId) {
        if (materialId == kNoneIndex) {
            return false;
        }
        const MaterialStore::MaterialInfo& info = materials.infos[materialId];
        return isLightEmitter(packMaterial(info.material, info.extension));
    };
    const uint32_t entityCount = assets.asset(AssetBuffer::Primitives).address == 0 ? 0 : storage.size();
    const std::span<const uint32_t> nodeInstanceFirsts = frameData.entityNodeInstanceFirst();
    std::vector<glm::vec2> centers;
    for (EntityId id = 0; id < entityCount; ++id) {
        // an entity whose load is not flushed yet has no GPU model and no node-instance rows
        if ((storage.flags[id] & EntityFlag::Active) == 0 || storage.modelIds[id] == kInvalidModelId) {
            continue;
        }
        const ModelId modelId = storage.modelIds[id];
        const ModelAllocation& model = assets.models()[modelId];
        // indexed by model-relative primitive row
        const std::span<const vk::DeviceAddress> blas = assets.rayGeometry().blasAddresses(modelId);
        const std::span<const uint8_t> micromapped = assets.rayGeometry().micromapFlags(modelId);
        const glm::mat4 entityMatrix = computeModelMatrix(storage.transforms[id]);
        const auto firstRow = static_cast<uint32_t>(templates.rows.size());
        BoundingBox entityLocalBounds{.bboxMin = glm::vec3(std::numeric_limits<float>::max()),
                                      .bboxMax = glm::vec3(std::numeric_limits<float>::lowest())};
        for (uint32_t k = 0; k < model.nodes.count; ++k) {
            const NodeDraw& node = geometry.nodeDraws[model.nodes.first + k];
            const glm::mat4 world = entityMatrix * node.localToModel;
            for (uint32_t p = node.firstPrimitive; p < node.firstPrimitive + node.primitiveCount; ++p) {
                const vk::DeviceAddress address = blas[p - model.primitives.first];
                // a primitive without meshlets has no BLAS
                if (address == 0) {
                    continue;
                }
                const PrimitiveDraw& primitive = geometry.primitiveDraws[p];
                const BoundingBox box = primitiveBounds(geometry, primitive);
                templates.rows.push_back(GpuRayInstance{.nodeInstanceIndex = nodeInstanceFirsts[id] + k,
                                                        .primitiveRow = p});
                templates.builds.push_back(GpuRayInstanceBuild{
                    .blas = address,
                    .flags = micromapped[p - model.primitives.first] != 0 ? GpuRayInstanceBuildFlag::OpacityMicromap
                                                                          : 0u,
                });
                if (isEmissive(primitive.materialId)) {
                    templates.firstLights.push_back(templates.triangleLightCount);
                    templates.lightTasks.push_back(GpuTriangleLightTask{
                        .firstLight = templates.triangleLightCount,
                        .triangleCount = primitive.meshlets.triangleCount,
                        .rayInstance = static_cast<uint32_t>(templates.rows.size() - 1),
                    });
                    templates.triangleLightCount += primitive.meshlets.triangleCount;
                } else {
                    templates.firstLights.push_back(kNoneIndex);
                }
                const glm::vec3 localCenter =
                    glm::vec3(node.localToModel * glm::vec4(0.5f * (box.bboxMin + box.bboxMax), 1.0f));
                templates.rowLocalCenters.push_back(localCenter);
                const glm::vec4 center = entityMatrix * glm::vec4(localCenter, 1.0f);
                centers.emplace_back(center.x, center.z);
                expandBounds(entityLocalBounds, node.localToModel, box);
                expandBounds(templates.sceneBounds, world, box);
            }
        }
        const auto rowCount = static_cast<uint32_t>(templates.rows.size()) - firstRow;
        if (rowCount > 0 || !model.lights.empty()) {
            templates.dynamicEntities.push_back(DynamicEntity{
                .id = id,
                .localBounds = entityLocalBounds,
                .lastTransform = storage.transforms[id],
                .firstRow = firstRow,
                .rowCount = rowCount,
            });
        }
    }

    const auto count = static_cast<uint32_t>(templates.rows.size());
    const uint64_t maxInstances = device.capabilities.accelerationStructure.maxInstanceCount;
    if (count > maxInstances) {
        throw std::runtime_error(std::format("rt: {} instances exceed maxInstanceCount {}", count, maxInstances));
    }
    if (assets.uploadedCount(AssetBuffer::Primitives) > kCustomIndexLimit) {
        throw std::runtime_error(std::format("rt: {} primitive rows exceed the 24-bit instance custom index",
                                             assets.uploadedCount(AssetBuffer::Primitives)));
    }

    // no global partition: changed instances are rewritten in their grid partition, which measured faster (feature
    // spec §3.6). MG-P5: refine a power-of-two XZ grid while any cell exceeds instancesPerPartition, drop empty cells,
    // and number occupied partitions in 2D Morton Z-order
    const uint32_t maxPartitions = device.capabilities.partitionedAccelerationStructure.maxPartitionCount;
    if (maxPartitions == 0) {
        throw std::runtime_error("rt: maxPartitionCount is 0");
    }
    if (count == 0) {
        templates.gridLower = glm::vec2(0.0f);
        templates.gridExtent = glm::vec2(1.0f);
        templates.gridAxis = 1;
        templates.cellToPartition = {0};
        templates.partitionOccupancy = {0};
        templates.shape = PtlasShape{.instances = 1, .partitions = 1, .maxPerPartition = 1};
    } else {
        glm::vec2 lower(std::numeric_limits<float>::max());
        glm::vec2 upper(std::numeric_limits<float>::lowest());
        for (const glm::vec2& center : centers) {
            lower = glm::min(lower, center);
            upper = glm::max(upper, center);
        }
        const glm::vec2 extent = glm::max(upper - lower, glm::vec2(std::numeric_limits<float>::min()));
        const uint32_t maxPartitionsLimit = std::min(maxPartitions, kDerivedMaxPartitions);
        const uint32_t targetPerPartition = std::max(templates.instancesPerPartition, 1u);
        const uint32_t targetCells = divideRoundUp(count, targetPerPartition);
        const uint32_t maxInstancesLimit = std::max(kDerivedMaxInstances, targetPerPartition);

        auto tallyGrid = [&](uint32_t candidateAxis, std::vector<uint32_t>& counts, uint32_t& occupied,
                             uint32_t& maxCell) {
            counts.assign(static_cast<size_t>(candidateAxis) * candidateAxis, 0);
            for (const glm::vec2& center : centers) {
                ++counts[mortonCellForCenter(center, lower, extent, candidateAxis)];
            }
            occupied = 0;
            maxCell = 0;
            for (const uint32_t n : counts) {
                if (n > 0) {
                    ++occupied;
                    maxCell = std::max(maxCell, n);
                }
            }
        };

        // pick the power-of-two grid closest to targetCells without exceeding maxPartitionsLimit, then refine only
        // when a dense pocket still exceeds maxInstancesLimit
        uint32_t axis = 1;
        while (axis * 2 <= kMaxGridAxis && (axis * 2) * (axis * 2) <= maxPartitionsLimit &&
               (axis * 2) * (axis * 2) <= targetCells * 2) {
            axis *= 2;
        }
        std::vector<uint32_t> cellCounts;
        uint32_t occupied = 0;
        uint32_t maxCell = 0;
        tallyGrid(axis, cellCounts, occupied, maxCell);
        std::vector<uint32_t> nextCounts;
        while (maxCell > maxInstancesLimit && axis * 2 <= kMaxGridAxis) {
            const uint32_t nextAxis = axis * 2;
            uint32_t nextOccupied = 0;
            uint32_t nextMaxCell = 0;
            tallyGrid(nextAxis, nextCounts, nextOccupied, nextMaxCell);
            if (nextOccupied > maxPartitionsLimit || nextMaxCell >= maxCell) {
                break;
            }
            axis = nextAxis;
            cellCounts.swap(nextCounts);
            occupied = nextOccupied;
            maxCell = nextMaxCell;
        }

        templates.gridLower = lower;
        templates.gridExtent = extent;
        templates.gridAxis = axis;
        templates.cellToPartition.resize(cellCounts.size());
        templates.partitionOccupancy.reserve(occupied);
        uint32_t lastPartition = 0;
        for (uint32_t m = 0; m < cellCounts.size(); ++m) {
            if (cellCounts[m] > 0) {
                lastPartition = static_cast<uint32_t>(templates.partitionOccupancy.size());
                templates.partitionOccupancy.push_back(cellCounts[m]);
            }
            templates.cellToPartition[m] = lastPartition;
        }
        for (uint32_t row = 0; row < count; ++row) {
            templates.builds[row].partition =
                templates.cellToPartition[mortonCellForCenter(centers[row], lower, extent, axis)];
        }
        templates.shape = PtlasShape{
            .instances = count,
            .partitions = occupied,
            .maxPerPartition = std::max(maxCell, 1u),
        };
    }
    logInfo(std::format("rt templates: {} instances, {} partitions ({}x{} grid, Z-order), at most {} per partition; {} "
                        "triangle lights in {} emissive instances",
                        count, templates.shape.partitions, templates.gridAxis, templates.gridAxis,
                        templates.shape.maxPerPartition, templates.triangleLightCount, templates.lightTasks.size()),
            "RayTracingScene");
}

void RayTracingScene::updateDynamicEntities()
{
    ZoneScopedN("RayTracingScene::updateDynamicEntities");
    const ObjectStorage& storage = scene.objectStorage;
    const bool shouldReassignPartitions = settings.isPartitionReassignmentEnabled && templates.gridAxis > 1;
    bool boundsGrew = false;
    bool partitionsChanged = false;
    for (DynamicEntity& entity : templates.dynamicEntities) {
        if (entity.id >= storage.size() || (storage.flags[entity.id] & EntityFlag::Active) == 0) {
            continue;
        }
        const Transform& current = storage.transforms[entity.id];
        if (current == entity.lastTransform) {
            continue;
        }
        entity.lastTransform = current;
        isSceneChangedThisFrame = true;
        if (entity.rowCount == 0) {
            continue;
        }
        const glm::mat4 entityMatrix = computeModelMatrix(current);

        // MG-B4: grow sceneBounds monotonically with hysteresis when a moved entity leaves its layout box
        BoundingBox worldBox{.bboxMin = glm::vec3(std::numeric_limits<float>::max()),
                             .bboxMax = glm::vec3(std::numeric_limits<float>::lowest())};
        expandBounds(worldBox, entityMatrix, entity.localBounds);
        if (glm::any(glm::lessThan(worldBox.bboxMin, templates.sceneBounds.bboxMin)) ||
            glm::any(glm::greaterThan(worldBox.bboxMax, templates.sceneBounds.bboxMax))) {
            const glm::vec3 extent =
                glm::max(templates.sceneBounds.bboxMax - templates.sceneBounds.bboxMin, glm::vec3(1.0f));
            const glm::vec3 margin = glm::max(extent * kDynamicBoundsGrowthFraction, glm::vec3(kDynamicBoundsMinMargin));
            for (int axis = 0; axis < 3; ++axis) {
                if (worldBox.bboxMin[axis] < templates.sceneBounds.bboxMin[axis]) {
                    templates.sceneBounds.bboxMin[axis] = worldBox.bboxMin[axis] - margin[axis];
                }
                if (worldBox.bboxMax[axis] > templates.sceneBounds.bboxMax[axis]) {
                    templates.sceneBounds.bboxMax[axis] = worldBox.bboxMax[axis] + margin[axis];
                }
            }
            boundsGrew = true;
        }

        // C-C4-5: reassign moved rows to their current XZ grid cell's compacted Z-order partition
        if (shouldReassignPartitions) {
            for (uint32_t row = entity.firstRow; row < entity.firstRow + entity.rowCount; ++row) {
                const glm::vec4 center = entityMatrix * glm::vec4(templates.rowLocalCenters[row], 1.0f);
                const uint32_t cell = mortonCellForCenter({center.x, center.z}, templates.gridLower,
                                                          templates.gridExtent, templates.gridAxis);
                const uint32_t newPartition = templates.cellToPartition[cell];
                uint32_t& oldPartition = templates.builds[row].partition;
                if (newPartition != oldPartition) {
                    --templates.partitionOccupancy[oldPartition];
                    ++templates.partitionOccupancy[newPartition];
                    oldPartition = newPartition;
                    partitionsChanged = true;
                    templates.shape.maxPerPartition =
                        std::max(templates.shape.maxPerPartition, templates.partitionOccupancy[newPartition]);
                }
            }
        }
    }
    if (boundsGrew) {
        logInfo(std::format("expanded dynamic scene bounds to ({:.1f}, {:.1f}, {:.1f})-({:.1f}, {:.1f}, {:.1f})",
                            templates.sceneBounds.bboxMin.x, templates.sceneBounds.bboxMin.y,
                            templates.sceneBounds.bboxMin.z, templates.sceneBounds.bboxMax.x,
                            templates.sceneBounds.bboxMax.y, templates.sceneBounds.bboxMax.z),
                "RayTracingScene");
    }
    if (partitionsChanged) {
        ++templates.buildsVersion;
    }
}

void RayTracingScene::uploadTemplates(uint32_t frameSlot)
{
    ZoneScopedN("RayTracingScene::uploadTemplates");
    FrameBuffers& buffers = frames[frameSlot];
    const auto count = static_cast<uint32_t>(templates.rows.size());
    if (count > buffers.capacity || buffers.rayInstances.memory == nullptr) {
        const uint64_t maxInstances = std::min<uint64_t>(device.capabilities.accelerationStructure.maxInstanceCount,
                                                         std::numeric_limits<uint32_t>::max());
        const uint64_t grownCapacity =
            std::max<uint64_t>({count, static_cast<uint64_t>(buffers.capacity) * 2, kMinCapacity});
        createFrameBuffers(frameSlot, static_cast<uint32_t>(std::min(grownCapacity, maxInstances)));
    }
    // write-combined memory: whole arrays, never read back
    std::memcpy(buffers.rayInstances.mapped, templates.rows.data(), std::span(templates.rows).size_bytes());
    std::memcpy(buffers.builds.mapped, templates.builds.data(), std::span(templates.builds).size_bytes());
    std::memcpy(buffers.rayInstanceLights.mapped, templates.firstLights.data(),
                std::span(templates.firstLights).size_bytes());
    std::memcpy(buffers.triangleLightTasks.mapped, templates.lightTasks.data(),
                std::span(templates.lightTasks).size_bytes());
    buffers.count = count;
    buffers.version = templates.version;
    buffers.buildsVersion = templates.buildsVersion;
    TracyPlot("RT/Instances", static_cast<double>(count));
}

vk::PartitionedAccelerationStructureInstancesInputNV RayTracingScene::buildInput(const PtlasShape& shape) const
{
    return vk::PartitionedAccelerationStructureInstancesInputNV{
        .flags = vk::BuildAccelerationStructureFlagBitsKHR::ePreferFastTrace,
        .instanceCount = shape.instances,
        .maxInstancePerPartitionCount = shape.maxPerPartition,
        .partitionCount = shape.partitions,
        .maxInstanceInGlobalPartitionCount = 0,
    };
}

void RayTracingScene::createPtlas(uint64_t lastSubmittedFrame)
{
    ZoneScopedN("RayTracingScene::createPtlas");
    // frames in flight trace the PTLAS and update it in place: let the last submitted ones finish
    if (lastSubmittedFrame > 0) {
        const std::array semaphores{*frameSync.computeTimeline, *frameSync.rayTimeline};
        const std::array values{lastSubmittedFrame, lastSubmittedFrame};
        const vk::SemaphoreWaitInfo waitInfo{.semaphoreCount = static_cast<uint32_t>(semaphores.size()),
                                             .pSemaphores = semaphores.data(),
                                             .pValues = values.data()};
        while (vk::Result::eTimeout == device.vkdevice.waitSemaphores(waitInfo, UINT64_MAX))
            ;
    }
    const PtlasShape previous = ptlas.sizedFor;
    destroyPtlas();

    // MG-B3: monotonic with headroom, clamped to maxInstanceCount so growth never exceeds the device limit; only
    // double instances when the instance count itself grew (not when maxPerPartition alone grew)
    const uint64_t maxInstances = std::min<uint64_t>(device.capabilities.accelerationStructure.maxInstanceCount,
                                                     std::numeric_limits<uint32_t>::max());
    const uint64_t grownInstances = templates.shape.instances > previous.instances
        ? std::max<uint64_t>({templates.shape.instances, static_cast<uint64_t>(previous.instances) * 2, kMinCapacity})
        : std::max<uint64_t>({templates.shape.instances, previous.instances, kMinCapacity});
    const uint32_t sizedInstances = static_cast<uint32_t>(std::min(grownInstances, maxInstances));
    const uint32_t sizedMaxPerPartition = templates.shape.maxPerPartition > previous.maxPerPartition &&
                                          previous.maxPerPartition > 0
        ? std::min(std::max(templates.shape.maxPerPartition, previous.maxPerPartition * 2), sizedInstances)
        : std::max(templates.shape.maxPerPartition, previous.maxPerPartition);
    const PtlasShape sized{
        .instances = sizedInstances,
        .partitions = std::max(templates.shape.partitions, previous.partitions),
        .maxPerPartition = sizedMaxPerPartition,
    };
    const vk::AccelerationStructureBuildSizesInfoKHR sizes =
        device.vkdevice.getPartitionedAccelerationStructuresBuildSizesNV(buildInput(sized));
    ptlas.storage = createAlignedBuffer(sizes.accelerationStructureSize, kPtlasAlignment,
                                        vk::BufferUsageFlagBits2::eAccelerationStructureStorageKHR, "PtlasStorage");
    ptlas.scratch = createAlignedBuffer(sizes.buildScratchSize, kPtlasAlignment,
                                        vk::BufferUsageFlagBits2::eStorageBuffer, "PtlasScratch");
    ptlas.state = createAlignedBuffer(kPtlasStateRowBytes * sized.instances, alignof(glm::vec4),
                                      vk::BufferUsageFlagBits2::eStorageBuffer, "PtlasState");
    ptlas.sizedFor = sized;
    // the first build starts from nothing
    ptlas.version = ~0u;
    logInfo(std::format("PTLAS for {} instances, {} partitions, {} per partition: {} KiB, scratch {} KiB",
                        sized.instances, sized.partitions, sized.maxPerPartition,
                        divideRoundUp(sizes.accelerationStructureSize, vk::DeviceSize{1024}),
                        divideRoundUp(sizes.buildScratchSize, vk::DeviceSize{1024})),
            "RayTracingScene");
}

void RayTracingScene::destroyPtlas()
{
    destroyAlignedBuffer(ptlas.storage);
    destroyAlignedBuffer(ptlas.scratch);
    destroyAlignedBuffer(ptlas.state);
    ptlas.sizedFor = {};
    ptlas.version = ~0u;
}

void RayTracingScene::createFrameBuffers(uint32_t frameSlot, uint32_t capacity)
{
    ZoneScopedN("RayTracingScene::createFrameBuffers");
    FrameBuffers& buffers = frames[frameSlot];
    destroyFrameBuffers(buffers);
    buffers.capacity = capacity;
    const VmaAllocator vma = allocator.allocator;

    buffers.rayInstances = createHostBuffer(sizeof(GpuRayInstance) * vk::DeviceSize{capacity}, vma, device.vkdevice,
                                            device.queueFamilyIndices, std::format("RayInstances_{}", frameSlot),
                                            kRayInstancesTracyName);
    buffers.builds = createHostBuffer(sizeof(GpuRayInstanceBuild) * vk::DeviceSize{capacity}, vma, device.vkdevice,
                                      device.queueFamilyIndices, std::format("RayInstanceBuilds_{}", frameSlot),
                                      kRayBuildsTracyName);
    buffers.rayInstanceLights = createHostBuffer(sizeof(uint32_t) * vk::DeviceSize{capacity}, vma, device.vkdevice,
                                                 device.queueFamilyIndices,
                                                 std::format("RayInstanceLights_{}", frameSlot),
                                                 kRayInstanceLightsTracyName);
    buffers.triangleLightTasks = createHostBuffer(sizeof(GpuTriangleLightTask) * vk::DeviceSize{capacity}, vma,
                                                  device.vkdevice, device.queueFamilyIndices,
                                                  std::format("TriangleLightTasks_{}", frameSlot),
                                                  kTriangleLightTasksTracyName);
    // the build reads the op and its count as build input; argCount is copied to argCountReadback for CPU telemetry
    buffers.operation = createHostBuffer(kOperationBytes, vma, device.vkdevice, device.queueFamilyIndices,
                                         std::format("PtlasOperation_{}", frameSlot), kPtlasOperationTracyName,
                                         vk::BufferUsageFlagBits2::eAccelerationStructureBuildInputReadOnlyKHR |
                                             vk::BufferUsageFlagBits2::eTransferSrc);
    // MG-B2: host-cached readback buffer avoids uncached WC VRAM reads in collectResults
    createBuffer(sizeof(uint32_t), vk::BufferUsageFlagBits2::eTransferDst,
                 vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent |
                     vk::MemoryPropertyFlagBits::eHostCached,
                 buffers.argCountReadback.buffer, buffers.argCountReadback.memory, vma, device.vkdevice,
                 device.queueFamilyIndices, std::format("PtlasArgCountReadback_{}", frameSlot),
                 VMA_ALLOCATION_CREATE_MAPPED_BIT);
    {
        VmaAllocationInfo readbackInfo{};
        vmaGetAllocationInfo(vma, buffers.argCountReadback.memory, &readbackInfo);
        buffers.argCountReadback.mapped = readbackInfo.pMappedData;
        *static_cast<uint32_t*>(buffers.argCountReadback.mapped) = 0;
        tracyResourceAlloc(static_cast<VkBuffer>(*buffers.argCountReadback.buffer), sizeof(uint32_t),
                           kPtlasArgCountReadbackTracyName);
    }
    buffers.writeInfos =
        createAlignedBuffer(sizeof(vk::PartitionedAccelerationStructureWriteInstanceDataNV) * vk::DeviceSize{capacity},
                            alignof(vk::DeviceAddress),
                            vk::BufferUsageFlagBits2::eStorageBuffer |
                                vk::BufferUsageFlagBits2::eAccelerationStructureBuildInputReadOnlyKHR,
                            std::format("PtlasWriteInfos_{}", frameSlot));
    logInfo(std::format("slot {}: capacity {} instances", frameSlot, capacity), "RayTracingScene");
}

void RayTracingScene::destroyFrameBuffers(FrameBuffers& buffers)
{
    destroyHostBuffer(allocator.allocator, buffers.rayInstances, kRayInstancesTracyName);
    destroyHostBuffer(allocator.allocator, buffers.builds, kRayBuildsTracyName);
    destroyHostBuffer(allocator.allocator, buffers.rayInstanceLights, kRayInstanceLightsTracyName);
    destroyHostBuffer(allocator.allocator, buffers.triangleLightTasks, kTriangleLightTasksTracyName);
    destroyHostBuffer(allocator.allocator, buffers.operation, kPtlasOperationTracyName);
    destroyHostBuffer(allocator.allocator, buffers.argCountReadback, kPtlasArgCountReadbackTracyName);
    destroyAlignedBuffer(buffers.writeInfos);
    buffers.capacity = 0;
    buffers.count = 0;
    buffers.version = ~0u;
    buffers.buildsVersion = ~0u;
    buffers.isSubmitted = false;
}

RayTracingScene::AlignedBuffer RayTracingScene::createAlignedBuffer(vk::DeviceSize bytes, vk::DeviceSize alignment,
                                                                    vk::BufferUsageFlags2 usage,
                                                                    std::string_view debugName) const
{
    AlignedBuffer target;
    // MG-B1: align the buffer allocation directly via VMA minAlignment
    createBuffer(std::max<vk::DeviceSize>(bytes, 1), usage | vk::BufferUsageFlagBits2::eShaderDeviceAddress,
                 vk::MemoryPropertyFlagBits::eDeviceLocal, target.buffer, target.memory, allocator.allocator,
                 device.vkdevice, device.queueFamilyIndices, debugName, 0, alignment);
    target.address = device.vkdevice.getBufferAddress({.buffer = *target.buffer});
    if (alignment > 1 && target.address % alignment != 0) {
        throw std::runtime_error(
            std::format("RayTracingScene: {} address {:#x} is not {}-byte aligned", debugName, target.address,
                        alignment));
    }
    return target;
}

void RayTracingScene::destroyAlignedBuffer(AlignedBuffer& target) const
{
    destroyVmaBuffer(allocator.allocator, target.buffer, target.memory);
    target.address = 0;
}

void RayTracingScene::recordBuild(const vk::raii::CommandBuffer& cmd, uint32_t frameSlot)
{
    ZoneScopedN("RayTracingScene::recordBuild");
    FrameBuffers& buffers = frames[frameSlot];
    const uint32_t firstQuery = frameSlot * kQueriesPerFrame;
    buffers.isTimed = timestampPeriodNs > 0.0f;
    buffers.isSubmitted = true;
    if (buffers.isTimed) {
        cmd.resetQueryPool(*timestamps, firstQuery, kQueriesPerFrame);
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *timestamps, firstQuery);
    }

    // last frame's build on this queue wrote the PTLAS and read the write infos; last frame's instance pass wrote the
    // state rows this pass reads and rewrites
    const vk::MemoryBarrier2 reuseBarrier{
        .srcStageMask =
            vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR | vk::PipelineStageFlagBits2::eComputeShader,
        .srcAccessMask = vk::AccessFlagBits2::eAccelerationStructureWriteKHR | vk::AccessFlagBits2::eShaderStorageWrite,
        .dstStageMask =
            vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR,
        .dstAccessMask = vk::AccessFlagBits2::eAccelerationStructureReadKHR |
            vk::AccessFlagBits2::eAccelerationStructureWriteKHR | vk::AccessFlagBits2::eShaderStorageRead |
            vk::AccessFlagBits2::eShaderStorageWrite,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &reuseBarrier});

    // a new layout or a new PTLAS: every row writes its instance into an empty PTLAS
    const bool isFullBuild = settings.isFullBuildForced || ptlas.version != templates.version;
    if (buffers.count > 0) {
        const RtInstancesPushData push{
            .frameRoot = frameData.frameRootAddress(frameSlot),
            .buildRows = buffers.builds.address,
            .writeInfos = buffers.writeInfos.address,
            .operation = buffers.operation.address,
            .ptlasState = ptlas.state.address,
            .count = buffers.count,
            .flags = isFullBuild ? RtInstancesFlag::FullBuild : 0u,
        };
        ShaderObjects::bindComputeShader(cmd, shaderObjects.rtShaders.writeInstances);
        cmd.pushDataEXT(vk::PushDataInfoEXT{.offset = 0, .data = {.address = &push, .size = sizeof(push)}});
        cmd.dispatch(divideRoundUp(buffers.count, kRtInstancesGroupSize), 1, 1);
        // the write infos and the op's argument count are build input (and argCount is copied for host telemetry)
        const vk::MemoryBarrier2 instanceBarrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask = vk::AccessFlagBits2::eShaderStorageWrite,
            .dstStageMask =
                vk::PipelineStageFlagBits2::eAccelerationStructureBuildKHR | vk::PipelineStageFlagBits2::eCopy,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eTransferRead,
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &instanceBarrier});
    }
    if (buffers.isTimed) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *timestamps, firstQuery + 1);
    }

    // an empty scene still builds: the frame root always points at a valid PTLAS. In place after the first build
    // (VUID-...-pBuildInfo-10549 forbids overlap only between distinct source and destination); every build of one
    // PTLAS uses the input it was sized for
    cmd.buildPartitionedAccelerationStructuresNV(vk::BuildPartitionedAccelerationStructureInfoNV{
        .input = buildInput(ptlas.sizedFor),
        .srcAccelerationStructureData = isFullBuild ? 0 : ptlas.storage.address,
        .dstAccelerationStructureData = ptlas.storage.address,
        .scratchData = ptlas.scratch.address,
        .srcInfos = buffers.operation.address,
        .srcInfosCount = buffers.operation.address + kOpCountOffset,
    });
    ptlas.version = templates.version;
    if (buffers.isTimed) {
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *timestamps, firstQuery + 2);
    }
    if (buffers.count > 0) {
        // MG-B2: copy the 4-byte argCount into host-cached memory for collectResults
        const vk::BufferCopy2 copyRegion{
            .srcOffset = kArgCountOffset,
            .dstOffset = 0,
            .size = sizeof(uint32_t),
        };
        cmd.copyBuffer2(vk::CopyBufferInfo2{
            .srcBuffer = *buffers.operation.buffer,
            .dstBuffer = *buffers.argCountReadback.buffer,
            .regionCount = 1,
            .pRegions = &copyRegion,
        });
        const vk::MemoryBarrier2 hostBarrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
            .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask = vk::AccessFlagBits2::eHostRead,
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &hostBarrier});
    }
}

void RayTracingScene::collectResults(uint32_t frameSlot)
{
    FrameBuffers& buffers = frames[frameSlot];
    if (buffers.isSubmitted) {
        buffers.isSubmitted = false;
        uint32_t argCount = 0;
        std::memcpy(&argCount, buffers.argCountReadback.mapped, sizeof(argCount));
        lastWrittenInstances = argCount;
        TracyPlot("RT/PTLAS instance writes", static_cast<double>(argCount));
    }
    if (!buffers.isTimed) {
        return;
    }
    buffers.isTimed = false;
    const auto [result, ticks] =
        timestamps.getResults<uint64_t>(frameSlot * kQueriesPerFrame, kQueriesPerFrame,
                                        kQueriesPerFrame * sizeof(uint64_t), sizeof(uint64_t),
                                        vk::QueryResultFlagBits::e64);
    if (result != vk::Result::eSuccess) {
        return;
    }
    const auto toMs = [this](uint64_t begin, uint64_t end) {
        return end > begin ? static_cast<float>(end - begin) * timestampPeriodNs * 1e-6f : 0.0f;
    };
    lastStartTick = ticks[0];
    lastEndTick = ticks[2];
    histories[0][historyHead] = toMs(ticks[0], ticks[1]);
    histories[1][historyHead] = toMs(ticks[1], ticks[2]);
    TracyPlot("RT instance write (async)", static_cast<double>(histories[0][historyHead]));
    TracyPlot("PTLAS build (async)", static_cast<double>(histories[1][historyHead]));
    historyHead = (historyHead + 1) % kHistorySize;
    historyCount = std::min(historyCount + 1, kHistorySize);
}

RayTracingScene::Stats RayTracingScene::getStats() const noexcept
{
    // unfilled slots are zero, so summing the whole ring is exact
    const auto average = [this](const std::array<float, kHistorySize>& samples) {
        return historyCount == 0
            ? 0.0f
            : std::reduce(samples.begin(), samples.end(), 0.0f) / static_cast<float>(historyCount);
    };
    return Stats{
        .instances = static_cast<uint32_t>(templates.rows.size()),
        .capacity = ptlas.sizedFor.instances,
        .writtenInstances = lastWrittenInstances,
        .partitions = templates.shape.partitions,
        .instanceWriteMs = average(histories[0]),
        .tlasBuildMs = average(histories[1]),
        .lastStartTick = lastStartTick,
        .lastEndTick = lastEndTick,
    };
}
