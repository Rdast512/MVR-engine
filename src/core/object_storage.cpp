#include "object_storage.hpp"
#include "util/vk_tracy.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cassert>
#include <format>

EntityId ObjectStorage::create(const Transform& transform, const MeshletDraw& meshletDraw,
                               const MaterialRef& material, uint32_t firstPrimitive, uint32_t primitiveCount,
                               std::string_view name)
{
    const auto id = static_cast<EntityId>(transforms.size());
    transforms.push_back(transform);
    modelMatrices.emplace_back(1.0f);
    prevModelMatrices.emplace_back(1.0f);
    meshletDraws.push_back(meshletDraw);
    materials.push_back(material);
    flags.push_back(EntityFlag::Active | EntityFlag::Dynamic);
    names.emplace_back(name);
    firstPrimitives.push_back(firstPrimitive);
    primitiveCounts.push_back(primitiveCount);

#ifdef TRACY_ENABLE
    const std::string msg = std::format(
        "Entity {} '{}' meshlets=[{}, {}) tex={}", id, name, meshletDraw.firstMeshlet,
        meshletDraw.firstMeshlet + meshletDraw.meshletCount, material.textureIndex);
    TracyMessage(msg.c_str(), msg.size());
#endif

    return id;
}

void ObjectStorage::clear() noexcept
{
    transforms.clear();
    modelMatrices.clear();
    prevModelMatrices.clear();
    meshletDraws.clear();
    materials.clear();
    flags.clear();
    names.clear();
    firstPrimitives.clear();
    primitiveCounts.clear();
}

glm::mat4 computeModelMatrix(const Transform& transform)
{
    glm::mat4 model{1.0f};
    model = glm::translate(model, transform.position);
    model = glm::rotate(model, transform.rotation.x, glm::vec3{1.0f, 0.0f, 0.0f});
    model = glm::rotate(model, transform.rotation.y, glm::vec3{0.0f, 1.0f, 0.0f});
    model = glm::rotate(model, transform.rotation.z, glm::vec3{0.0f, 0.0f, 1.0f});
    model = glm::scale(model, transform.scale);
    return model;
}

void applyYawSpin(std::span<Transform> transforms, float deltaYawRadians)
{
    for (Transform& t : transforms) {
        t.rotation.y += deltaYawRadians;
    }
}

void writeObjectUbs(ObjectStorage& storage, std::span<GpuObjectUB> mappedUbs, const glm::mat4& meshPreRotation)
{
    const uint32_t count = storage.size();
    assert(mappedUbs.size() >= count);

    for (uint32_t i = 0; i < count; ++i) {
        if ((storage.flags[i] & EntityFlag::Active) == 0) {
            continue;
        }

        const glm::mat4 model = computeModelMatrix(storage.transforms[i]) * meshPreRotation;
        mappedUbs[i] = GpuObjectUB{
            .modelMatrix = model,
            .prevModelMatrix = storage.prevModelMatrices[i],
            .materialID = storage.materials[i].materialId,
            .instanceFlags = storage.flags[i],
        };
        storage.prevModelMatrices[i] = model;
        storage.modelMatrices[i] = model;
    }
}
