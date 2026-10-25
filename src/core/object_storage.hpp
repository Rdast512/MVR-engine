#pragma once

#include "types.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// ---------------------------------------------------------------------------
// Dense entity index into ObjectStorage SoA columns. Not a generational handle.
// ---------------------------------------------------------------------------
using EntityId = uint32_t;
inline constexpr EntityId kInvalidEntityId = ~EntityId{0};
// index into ResourceManager::modelAllocations; several entities may draw one model
using ModelId = uint32_t;
inline constexpr ModelId kInvalidModelId = ~ModelId{0};

struct Transform
{
    glm::vec3 position{0.0f, 0.0f, 0.0f};
    glm::vec3 rotation{0.0f, 0.0f, 0.0f}; // radians, XYZ Euler (matches prior Object)
    glm::vec3 scale{1.0f, 1.0f, 1.0f};
};


namespace EntityFlag
{
    inline constexpr uint32_t None = 0;
    inline constexpr uint32_t Active = 1u << 0;
    inline constexpr uint32_t Dynamic = 1u << 1;
} // namespace EntityFlag

// ---------------------------------------------------------------------------
// ObjectStorage — SoA world instances. No Vulkan handles here.
// Columns are public for direct span-friendly access (DOD).
// ---------------------------------------------------------------------------
class ObjectStorage
{
public:
    std::vector<Transform> transforms;
    // last frame's model matrix, the GpuObjectUB prevModelMatrix source
    std::vector<glm::mat4> prevModelMatrices;
    std::vector<MeshletDraw> meshletDraws;
    std::vector<uint32_t> flags;
    std::vector<std::string> names;
    std::vector<uint32_t> firstPrimitives;
    std::vector<uint32_t> primitiveCounts;
    // model-space bounding sphere (xyz center, w radius), set when the model is flushed
    std::vector<glm::vec4> localBounds;
    // shared GPU model the entity draws; kInvalidModelId until its load is flushed
    std::vector<ModelId> modelIds;

    [[nodiscard]] EntityId create(const Transform& transform, const MeshletDraw& meshletDraw, uint32_t firstPrimitive,
                                  uint32_t primitiveCount, std::string_view name = {});

    [[nodiscard]] uint32_t size() const noexcept { return static_cast<uint32_t>(transforms.size()); }
    [[nodiscard]] bool empty() const noexcept { return transforms.empty(); }

    // order-preserving: later entities shift down by one id
    void erase(EntityId id);

    void clear() noexcept;
};

// ---------------------------------------------------------------------------
// Systems (span-based; safe for future parallel ranges)
// ---------------------------------------------------------------------------

[[nodiscard]] glm::mat4 computeModelMatrix(const Transform& transform);
// radius scales by the largest axis, so the sphere stays conservative under non-uniform scale
[[nodiscard]] glm::vec4 transformBoundingSphere(const glm::mat4& matrix, const glm::vec4& sphere);