#include "geometry_store.hpp"

#include "../static_headers/logger.hpp"
#include "../util/vk_tracy.hpp"

#include <meshoptimizer.h>

#include <format>

namespace
{
    constexpr size_t kMeshletMaxVertices = 64;
    constexpr size_t kMeshletMaxTriangles = 126;
    constexpr float kMeshletConeWeight = 0.0f;
} // namespace

void GeometryStore::resizeVertices(uint32_t newCount)
{
    const uint32_t oldCount = static_cast<uint32_t>(positions.size());
    if (newCount <= oldCount) {
        return;
    }

    positions.resize(newCount, glm::vec3{0.0f});
    normals.resize(newCount, glm::vec3{0.0f});
    tangents.resize(newCount, glm::vec4{0.0f});
    uv0.resize(newCount, glm::vec2{0.0f});
    uv1.resize(newCount, glm::vec2{0.0f});
    colors.resize(newCount, glm::vec4{1.0f});
    joints0.resize(newCount, std::array<uint16_t, 4>{0, 0, 0, 0});
    weights0.resize(newCount, glm::vec4{0.0f});
    vertices.resize(newCount);
}

void GeometryStore::packVertex(uint32_t v)
{
    GpuVertex packed{};
    packed.pos = positions[v];
    packed.color = glm::vec3{colors[v]};
    packed.texCoord = uv0[v];
    vertices[v] = packed;
}

MeshletDraw GeometryStore::buildMeshletsForRange(uint32_t firstIndex, uint32_t indexCount)
{
    ZoneScopedN("GeometryStore::buildMeshletsForRange");
    if (indexCount == 0 || vertices.empty() || firstIndex + indexCount > indices.size()) {
        return {};
    }

    const size_t maxMeshlets = meshopt_buildMeshletsBound(indexCount, kMeshletMaxVertices, kMeshletMaxTriangles);
    std::vector<meshopt_Meshlet> built(maxMeshlets);
    std::vector<unsigned int> localVertices(indexCount);
    std::vector<unsigned char> localTriangles(indexCount);

    const size_t meshletCount =
        meshopt_buildMeshlets(built.data(), localVertices.data(), localTriangles.data(), indices.data() + firstIndex,
                              indexCount, &vertices[0].pos.x, vertices.size(), sizeof(GpuVertex), kMeshletMaxVertices,
                              kMeshletMaxTriangles, kMeshletConeWeight);

    if (meshletCount == 0) {
        return {};
    }

    const meshopt_Meshlet& last = built[meshletCount - 1];
    localVertices.resize(last.vertex_offset + last.vertex_count);
    localTriangles.resize(last.triangle_offset + last.triangle_count * 3);
    built.resize(meshletCount);

    const uint32_t baseVertexOffset = static_cast<uint32_t>(meshletVertices.size());
    const uint32_t baseTriangleOffset = static_cast<uint32_t>(meshletTriangles.size());
    const uint32_t baseMeshlet = static_cast<uint32_t>(meshlets.size());

    meshletVertices.insert(meshletVertices.end(), localVertices.begin(), localVertices.end());
    meshletTriangles.insert(meshletTriangles.end(), localTriangles.begin(), localTriangles.end());
    meshlets.reserve(meshlets.size() + meshletCount);

    for (size_t i = 0; i < meshletCount; ++i) {
        const meshopt_Meshlet& m = built[i];
        const uint32_t vertexOffset = baseVertexOffset + m.vertex_offset;
        const uint32_t triangleOffset = baseTriangleOffset + m.triangle_offset;

        meshopt_optimizeMeshlet(meshletVertices.data() + vertexOffset, meshletTriangles.data() + triangleOffset,
                                m.triangle_count, m.vertex_count);

        const meshopt_Bounds bounds = meshopt_computeMeshletBounds(
            meshletVertices.data() + vertexOffset, meshletTriangles.data() + triangleOffset, m.triangle_count,
            &vertices[0].pos.x, vertices.size(), sizeof(GpuVertex));

        meshlets.push_back(GpuMeshletDesc{
            .vertexOffset = vertexOffset,
            .triangleOffset = triangleOffset,
            .vertexCount = m.vertex_count,
            .triangleCount = m.triangle_count,
            .boundingSphere = glm::vec4{bounds.center[0], bounds.center[1], bounds.center[2], bounds.radius},
        });
    }

    const MeshletDraw draw{
        .firstMeshlet = baseMeshlet,
        .meshletCount = static_cast<uint32_t>(meshletCount),
    };

    log_info(std::format("Built {} meshlets for index range [{}, {}) ({} meshlet verts, {} local tri corners)",
                         draw.meshletCount, firstIndex, firstIndex + indexCount, localVertices.size(),
                         localTriangles.size()),
             "AssetLoader");

    return draw;
}
