#pragma once

#include <glm/vec3.hpp>

// the sunlit Sponza courtyard view of the GI check, the temporal checks and ENGINE_BENCH_POSE=courtyard; the gallery
// pose is the startup camera
struct CourtyardPose
{
    glm::vec3 position;
    glm::vec3 target;
};
inline constexpr CourtyardPose kCourtyardPose{{6.0f, 2.5f, 1.5f}, {-10.0f, 1.0f, -1.0f}};
