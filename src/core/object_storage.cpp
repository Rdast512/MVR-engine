#include "object_storage.hpp"
#include "util/vk_tracy.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cassert>
#include <format>

EntityId ObjectStorage::create(const Transform& transform, const MeshletDraw& meshletDraw, uint32_t firstPrimitive,
                               uint32_t primitiveCount, std::string_view name)
{
    const auto id = static_cast<EntityId>(transforms.size());
    transforms.push_back(transform);
    prevModelMatrices.emplace_back(1.0f);
    meshletDraws.push_back(meshletDraw);
    flags.push_back(EntityFlag::Active | EntityFlag::Dynamic);
    names.emplace_back(name);
    firstPrimitives.push_back(firstPrimitive);
    primitiveCounts.push_back(primitiveCount);
    textureRefs.emplace_back();

#ifdef TRACY_ENABLE
    const std::string msg = std::format(
        "Entity {} '{}' meshlets=[{}, {}) primitives=[{}, {})", id, name, meshletDraw.firstMeshlet,
        meshletDraw.firstMeshlet + meshletDraw.meshletCount, firstPrimitive, firstPrimitive + primitiveCount);
    TracyMessage(msg.c_str(), msg.size());
#endif

    return id;
}

void ObjectStorage::clear() noexcept
{
    transforms.clear();
    prevModelMatrices.clear();
    meshletDraws.clear();
    flags.clear();
    names.clear();
    firstPrimitives.clear();
    primitiveCounts.clear();
    textureRefs.clear();
}

void ObjectStorage::erase(EntityId id)
{
    assert(id < size());
    const auto eraseAt = [id](auto& column) { column.erase(column.begin() + id); };
    eraseAt(transforms);
    eraseAt(prevModelMatrices);
    eraseAt(meshletDraws);
    eraseAt(flags);
    eraseAt(names);
    eraseAt(firstPrimitives);
    eraseAt(primitiveCounts);
    eraseAt(textureRefs);
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
