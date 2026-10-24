#include "object_storage.hpp"
#include "util/vk_tracy.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
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
    localBounds.emplace_back(0.0f);
    modelIds.push_back(kInvalidModelId);

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
    localBounds.clear();
    modelIds.clear();
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
    eraseAt(localBounds);
    eraseAt(modelIds);
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

glm::vec4 transformBoundingSphere(const glm::mat4& matrix, const glm::vec4& sphere)
{
    const float maxScale = std::sqrt(std::max({glm::dot(glm::vec3(matrix[0]), glm::vec3(matrix[0])),
                                               glm::dot(glm::vec3(matrix[1]), glm::vec3(matrix[1])),
                                               glm::dot(glm::vec3(matrix[2]), glm::vec3(matrix[2]))}));
    return {glm::vec3(matrix * glm::vec4(glm::vec3(sphere), 1.0f)), sphere.w * maxScale};
}

glm::vec4 mergeBoundingSpheres(const glm::vec4& a, const glm::vec4& b)
{
    if (a.w <= 0.0f) {
        return b;
    }
    if (b.w <= 0.0f) {
        return a;
    }
    const glm::vec3 offset = glm::vec3(b) - glm::vec3(a);
    const float distance = glm::length(offset);
    if (distance + b.w <= a.w) {
        return a;
    }
    if (distance + a.w <= b.w) {
        return b;
    }
    const float radius = (distance + a.w + b.w) * 0.5f;
    return {glm::vec3(a) + offset * ((radius - a.w) / distance), radius};
}
