#pragma once

#include "../core/geometry_store.hpp"
#include "../core/light_store.hpp"
#include "../core/material_store.hpp"
#include "../core/object_storage.hpp"

#include <glm/glm.hpp>

// ---------------------------------------------------------------------------
// Scene - world container: object SoA storage, origin, base axes.
// ---------------------------------------------------------------------------
class Scene
{
public:
    Scene() = default;
    ~Scene() = default;

    Scene(Scene&&) noexcept = default;
    Scene& operator=(Scene&&) noexcept = default;

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    ObjectStorage objectStorage;
    GeometryStore geometryStore;
    MaterialStore materialStore;
    LightStore lightStore;

    [[nodiscard]] const glm::vec3& getStartPosition() const noexcept { return startPosition; }
    void setStartPosition(const glm::vec3& pos) noexcept { startPosition = pos; }

    void setBaseAxes(const glm::vec3& right, const glm::vec3& upDir, const glm::vec3& forward) noexcept;

    glm::vec3 startPosition{0.0f};
};
