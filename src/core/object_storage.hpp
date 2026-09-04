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
    std::vector<glm::mat4> modelMatrices;
    std::vector<glm::mat4> prevModelMatrices;
    std::vector<MeshletDraw> meshletDraws;
    std::vector<MaterialRef> materials;
    std::vector<uint32_t> flags;
    std::vector<std::string> names;
    std::vector<uint32_t> firstPrimitives;
    std::vector<uint32_t> primitiveCounts;

    [[nodiscard]] EntityId create(const Transform& transform, const MeshletDraw& meshletDraw,
                                  const MaterialRef& material, uint32_t firstPrimitive, uint32_t primitiveCount,
                                  std::string_view name = {});

    [[nodiscard]] uint32_t size() const noexcept { return static_cast<uint32_t>(transforms.size()); }
    [[nodiscard]] bool empty() const noexcept { return transforms.empty(); }

    void clear() noexcept;
};

// ---------------------------------------------------------------------------
// Systems (span-based; safe for future parallel ranges)
// ---------------------------------------------------------------------------

[[nodiscard]] glm::mat4 computeModelMatrix(const Transform& transform);

// Demo / gameplay spin on Y (radians per call).
void applyYawSpin(std::span<Transform> transforms, float deltaYawRadians);

// Writes GpuObjectUB[i] for each active entity; updates prevModelMatrices for next frame.
// meshPreRotation is applied as: model = trs * meshPreRotation (same order as before).
void writeObjectUbs(ObjectStorage& storage, std::span<GpuObjectUB> mappedUbs, const glm::mat4& meshPreRotation);
