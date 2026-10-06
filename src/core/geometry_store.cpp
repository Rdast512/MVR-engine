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

void GeometryStore::reserveGeometry(size_t extraVertices, size_t extraIndices)
{
    const size_t vertexTarget = positions.size() + extraVertices;
    positions.reserve(vertexTarget);
    normals.reserve(vertexTarget);
    tangents.reserve(vertexTarget);
    uv0.reserve(vertexTarget);
    uv1.reserve(vertexTarget);
    colors.reserve(vertexTarget);
    joints0.reserve(vertexTarget);
    weights0.reserve(vertexTarget);
    vertices.reserve(vertexTarget);
    indices.reserve(indices.size() + extraIndices);
}

void GeometryStore::packVertex(uint32_t v)
{
    GpuVertex packed{};
    packed.pos = positions[v];
    packed.color = glm::vec3{colors[v]};
    packed.texCoord = uv0[v];
    vertices[v] = packed;
}

MeshletBuild GeometryStore::buildMeshlets(uint32_t firstIndex, uint32_t indexCount, uint32_t firstVertex,
                                          uint32_t vertexCount) const
{
    ZoneScopedN("GeometryStore::buildMeshlets");
    if (indexCount == 0 || vertexCount == 0 || firstIndex + indexCount > indices.size() ||
        firstVertex + vertexCount > vertices.size()) {
        return {};
    }

    // meshopt tables scale with vertex_count; feed only this primitive's range
    std::vector<unsigned int> rangeIndices(indexCount);
    for (uint32_t i = 0; i < indexCount; ++i) {
        const uint32_t index = indices[firstIndex + i];
        if (index < firstVertex || index - firstVertex >= vertexCount) {
            return {};
        }
        rangeIndices[i] = index - firstVertex;
    }
    const float* rangePositions = &vertices[firstVertex].pos.x;

    const size_t maxMeshlets = meshopt_buildMeshletsBound(indexCount, kMeshletMaxVertices, kMeshletMaxTriangles);
    std::vector<meshopt_Meshlet> built(maxMeshlets);
    std::vector<unsigned int> localVertices(indexCount);
    std::vector<unsigned char> localTriangles(indexCount);

    const size_t meshletCount =
        meshopt_buildMeshlets(built.data(), localVertices.data(), localTriangles.data(), rangeIndices.data(),
                              indexCount, rangePositions, vertexCount, sizeof(GpuVertex), kMeshletMaxVertices,
                              kMeshletMaxTriangles, kMeshletConeWeight);

    if (meshletCount == 0) {
        return {};
    }

    const meshopt_Meshlet& last = built[meshletCount - 1];
    localVertices.resize(last.vertex_offset + last.vertex_count);
    localTriangles.resize(last.triangle_offset + last.triangle_count * 3);

    MeshletBuild build{.meshlets = {}, .vertices = std::move(localVertices), .triangles = std::move(localTriangles)};
    build.meshlets.reserve(meshletCount);

    for (size_t i = 0; i < meshletCount; ++i) {
        const meshopt_Meshlet& m = built[i];
        meshopt_optimizeMeshlet(build.vertices.data() + m.vertex_offset, build.triangles.data() + m.triangle_offset,
                                m.triangle_count, m.vertex_count);

        const meshopt_Bounds bounds = meshopt_computeMeshletBounds(
            build.vertices.data() + m.vertex_offset, build.triangles.data() + m.triangle_offset, m.triangle_count,
            rangePositions, vertexCount, sizeof(GpuVertex));

        build.meshlets.push_back(GpuMeshletDesc{
            .vertexOffset = m.vertex_offset,
            .triangleOffset = m.triangle_offset,
            .vertexCount = m.vertex_count,
            .triangleCount = m.triangle_count,
            .boundingSphere = glm::vec4{bounds.center[0], bounds.center[1], bounds.center[2], bounds.radius},
        });
    }

    // rebase range-local vertex ids to scratch-absolute after optimize/bounds used them
    for (uint32_t& vertex : build.vertices) {
        vertex += firstVertex;
    }

    log_debug(std::format("Built {} meshlets for index range [{}, {}) ({} meshlet verts, {} local tri corners)",
                          build.meshlets.size(), firstIndex, firstIndex + indexCount, build.vertices.size(),
                          build.triangles.size()),
              "AssetLoader");
    return build;
}

MeshletDraw GeometryStore::appendMeshlets(MeshletBuild&& build)
{
    if (build.meshlets.empty()) {
        return {};
    }
    const uint32_t baseVertexOffset = static_cast<uint32_t>(meshletVertices.size());
    const uint32_t baseTriangleOffset = static_cast<uint32_t>(meshletTriangles.size());
    const MeshletDraw draw{
        .firstMeshlet = static_cast<uint32_t>(meshlets.size()),
        .meshletCount = static_cast<uint32_t>(build.meshlets.size()),
    };

    meshletVertices.insert(meshletVertices.end(), build.vertices.begin(), build.vertices.end());
    meshletTriangles.insert(meshletTriangles.end(), build.triangles.begin(), build.triangles.end());
    for (GpuMeshletDesc& meshlet : build.meshlets) {
        meshlet.vertexOffset += baseVertexOffset;
        meshlet.triangleOffset += baseTriangleOffset;
    }
    meshlets.insert(meshlets.end(), build.meshlets.begin(), build.meshlets.end());
    return draw;
}

MeshletDraw GeometryStore::buildMeshletsForRange(uint32_t firstIndex, uint32_t indexCount, uint32_t firstVertex,
                                                 uint32_t vertexCount)
{
    return appendMeshlets(buildMeshlets(firstIndex, indexCount, firstVertex, vertexCount));
}

void GeometryStore::clearScratch()
{
    auto drop = [](auto& vec) {
        vec.clear();
        vec.shrink_to_fit();
    };
    drop(positions);
    drop(normals);
    drop(tangents);
    drop(uv0);
    drop(uv1);
    drop(colors);
    drop(joints0);
    drop(weights0);
    drop(indices);
    drop(vertices);
    drop(meshlets);
    drop(meshletVertices);
    drop(meshletTriangles);
    drop(morphPos);
    drop(morphNrm);
    drop(morphTan);
    flushedPrimitiveCount = static_cast<uint32_t>(primitiveDraws.size());
    flushedMorphTargetCount = static_cast<uint32_t>(morphTargets.size());
}
