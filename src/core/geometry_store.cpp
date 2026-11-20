#include "geometry_store.hpp"

#include "../static_headers/logger.hpp"
#include "../util/vk_tracy.hpp"

#include <meshoptimizer.h>

#include <algorithm>
#include <array>
#include <format>
#include <limits>

namespace
{
    // level 0 of clusterlod.h's clodDefaultConfig (vk_lod_clusters raster preset), so these meshlets stay the base
    // level once LOD levels are built above them: Flex builder, min = max / 3, no cone weight, split factor 2
    constexpr size_t kMeshletMaxVertices = 64;
    constexpr size_t kMeshletMaxTriangles = 126;
    constexpr size_t kMeshletMinTriangles = kMeshletMaxTriangles / 3;
    static_assert(kMeshletMaxVertices <= UINT8_MAX && kMeshletMaxTriangles <= UINT8_MAX, "GpuMeshletDesc stores 8-bit counts");
    constexpr float kMeshletConeWeight = 0.0f;
    constexpr float kMeshletSplitFactor = 2.0f;
    constexpr int kMeshletOptimizeLevel = 1;
    // meshopt_partitionClusters may exceed its target by a third; the largest target that stays within a group
    constexpr size_t kGroupPartitionTarget = kMeshletGroupMaxSize * 3 / 4;
    static_assert(kGroupPartitionTarget + kGroupPartitionTarget / 3 <= kMeshletGroupMaxSize);

    struct MeshletPartition
    {
        std::vector<uint32_t> order;   // built meshlet index per output slot, groups contiguous
        std::vector<uint32_t> offsets; // group g covers order[offsets[g], offsets[g + 1])
    };

    // clusterlod.h partition(): meshlets sharing positions or close in space go together (spatial, sorted); a
    // primitive with at most kGroupPartitionTarget meshlets is one group
    MeshletPartition partitionMeshlets(const std::vector<GpuMeshletDesc>& meshlets,
                                       const std::vector<uint32_t>& localVertices,
                                       const std::vector<uint8_t>& localTriangles, const float* positions,
                                       uint32_t vertexCount)
    {
        const auto count = static_cast<uint32_t>(meshlets.size());
        MeshletPartition partition;
        partition.order.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            partition.order[i] = i;
        }
        if (count <= kGroupPartitionTarget) {
            partition.offsets = {0, count};
            return partition;
        }

        // vertices split on attribute seams share a position: connect them through a position remap
        std::vector<unsigned int> remap(vertexCount);
        meshopt_generatePositionRemap(remap.data(), positions, vertexCount, sizeof(GpuVertex));
        std::vector<unsigned int> clusterCounts(count);
        std::vector<unsigned int> clusterIndices;
        for (uint32_t i = 0; i < count; ++i) {
            const GpuMeshletDesc& meshlet = meshlets[i];
            clusterCounts[i] = meshlet.triangleCount * 3u;
            for (uint32_t c = 0; c < clusterCounts[i]; ++c) {
                clusterIndices.push_back(
                    remap[localVertices[meshlet.vertexOffset + localTriangles[meshlet.triangleOffset + c]]]);
            }
        }
        std::vector<unsigned int> part(count);
        const size_t partCount =
            meshopt_partitionClusters(part.data(), clusterIndices.data(), clusterIndices.size(), clusterCounts.data(),
                                      count, positions, vertexCount, sizeof(GpuVertex), kGroupPartitionTarget);

        // spatial order of the groups; any point of a group will do, clusterlod.h takes its last meshlet's center
        std::vector<float> points(partCount * 3);
        for (uint32_t i = 0; i < count; ++i) {
            std::copy_n(&meshlets[i].boundingSphere.x, 3, &points[part[i] * 3]);
        }
        std::vector<unsigned int> partRemap(partCount);
        meshopt_spatialSortRemap(partRemap.data(), points.data(), partCount, sizeof(float) * 3);

        // stable counting sort by group
        std::vector<uint32_t> offsets(partCount + 1, 0);
        for (uint32_t i = 0; i < count; ++i) {
            part[i] = partRemap[part[i]];
            ++offsets[part[i] + 1];
        }
        for (size_t g = 1; g <= partCount; ++g) {
            offsets[g] += offsets[g - 1];
        }
        std::vector<uint32_t> write(offsets.begin(), offsets.end() - 1);
        for (uint32_t i = 0; i < count; ++i) {
            partition.order[write[part[i]]++] = i;
        }

        // the bound is documented, not enforced: split any oversized group so a record never exceeds the limit
        partition.offsets.push_back(0);
        for (size_t g = 0; g < partCount; ++g) {
            for (uint32_t first = offsets[g]; first < offsets[g + 1]; first += kMeshletGroupMaxSize) {
                partition.offsets.push_back(std::min(first + kMeshletGroupMaxSize, offsets[g + 1]));
            }
        }
        return partition;
    }

    // box and edge range of one meshlet, from the corners it references (local ids into positions)
    GpuMeshletBounds computeMeshletBounds(const uint32_t* meshletVertices, const uint8_t* triangles,
                                          uint32_t triangleCount, const GpuVertex* positions)
    {
        GpuMeshletBounds bounds{
            .bboxMin = glm::vec3{std::numeric_limits<float>::max()},
            .shortestEdge = std::numeric_limits<float>::max(),
            .bboxMax = glm::vec3{std::numeric_limits<float>::lowest()},
            .longestEdge = 0.0f,
        };
        for (uint32_t t = 0; t < triangleCount; ++t) {
            std::array<glm::vec3, 3> corners{};
            for (uint32_t c = 0; c < 3; ++c) {
                corners[c] = positions[meshletVertices[triangles[t * 3 + c]]].pos;
                bounds.bboxMin = glm::min(bounds.bboxMin, corners[c]);
                bounds.bboxMax = glm::max(bounds.bboxMax, corners[c]);
            }
            for (uint32_t e = 0; e < 3; ++e) {
                const float length = glm::distance(corners[e], corners[(e + 1) % 3]);
                bounds.shortestEdge = std::min(bounds.shortestEdge, length);
                bounds.longestEdge = std::max(bounds.longestEdge, length);
            }
        }
        return bounds;
    }

    template <typename T>
    std::vector<T> expandToCorners(const std::vector<T>& stream, uint32_t firstVertex,
                                   const std::vector<uint32_t>& localIndices)
    {
        std::vector<T> corners;
        corners.reserve(localIndices.size());
        for (const uint32_t local : localIndices) {
            corners.push_back(stream[firstVertex + local]);
        }
        return corners;
    }

    template <typename T>
    void weldStream(std::vector<T>& stream, uint32_t firstVertex, size_t uniqueCount, const std::vector<T>& corners,
                    const std::vector<unsigned int>& remap)
    {
        stream.resize(firstVertex + uniqueCount);
        meshopt_remapVertexBuffer(stream.data() + firstVertex, corners.data(), corners.size(), sizeof(T), remap.data());
    }

    // experimental in meshoptimizer v1.3: the only call site
    void generateFlatNormals(std::vector<glm::vec3>& cornerNormals, const std::vector<glm::vec3>& cornerPositions)
    {
        constexpr float kFlatCreaseAngle = 0.0f;
        constexpr float kNoSmoothing = 0.0f;
        meshopt_generateNormals(&cornerNormals[0].x, nullptr, cornerNormals.size(), &cornerPositions[0].x,
                                cornerPositions.size(), sizeof(glm::vec3), kFlatCreaseAngle, kNoSmoothing);
    }

    // glTF v points down, normal maps +Y up: only 1 - v gives w of file-provided tangents
    void generateCompatibleTangents(std::vector<glm::vec4>& cornerTangents, const std::vector<glm::vec3>& cornerPositions,
                                    const std::vector<glm::vec3>& cornerNormals, std::vector<glm::vec2> uvs)
    {
        for (glm::vec2& uv : uvs) {
            uv.y = 1.0f - uv.y;
        }
        meshopt_generateTangents(&cornerTangents[0].x, nullptr, cornerTangents.size(), &cornerPositions[0].x,
                                 cornerPositions.size(), sizeof(glm::vec3), &cornerNormals[0].x, sizeof(glm::vec3),
                                 &uvs[0].x, sizeof(glm::vec2), meshopt_TangentCompatible);
    }
} // namespace

uint32_t GeometryStore::generateShadingAttributes(uint32_t firstVertex, std::vector<uint32_t>& localIndices,
                                                  const ShadingAttributeRequest& request)
{
    std::vector<glm::vec3> cornerPositions = expandToCorners(positions, firstVertex, localIndices);
    std::vector<glm::vec3> cornerNormals = expandToCorners(normals, firstVertex, localIndices);
    std::vector<glm::vec4> cornerTangents = expandToCorners(tangents, firstVertex, localIndices);
    std::vector<glm::vec2> cornerUv0 = expandToCorners(uv0, firstVertex, localIndices);
    std::vector<glm::vec2> cornerUv1 = expandToCorners(uv1, firstVertex, localIndices);
    std::vector<glm::vec4> cornerColors = expandToCorners(colors, firstVertex, localIndices);

    if (request.needsNormals) {
        generateFlatNormals(cornerNormals, cornerPositions);
        presentStreams |= GpuModelAttribute::Normals;
    }
    if (request.needsTangents) {
        generateCompatibleTangents(cornerTangents, cornerPositions, cornerNormals,
                                   request.tangentUvSet == 0 ? cornerUv0 : cornerUv1);
        presentStreams |= GpuModelAttribute::Tangents;
    }

    const size_t cornerCount = localIndices.size();
    const std::array streams = {
        meshopt_Stream{cornerPositions.data(), sizeof(glm::vec3), sizeof(glm::vec3)},
        meshopt_Stream{cornerNormals.data(), sizeof(glm::vec3), sizeof(glm::vec3)},
        meshopt_Stream{cornerTangents.data(), sizeof(glm::vec4), sizeof(glm::vec4)},
        meshopt_Stream{cornerUv0.data(), sizeof(glm::vec2), sizeof(glm::vec2)},
        meshopt_Stream{cornerUv1.data(), sizeof(glm::vec2), sizeof(glm::vec2)},
        meshopt_Stream{cornerColors.data(), sizeof(glm::vec4), sizeof(glm::vec4)},
    };
    std::vector<unsigned int> remap(cornerCount);
    const size_t uniqueCount = meshopt_generateVertexRemapMulti(remap.data(), nullptr, cornerCount, cornerCount,
                                                                streams.data(), streams.size());

    weldStream(positions, firstVertex, uniqueCount, cornerPositions, remap);
    weldStream(normals, firstVertex, uniqueCount, cornerNormals, remap);
    weldStream(tangents, firstVertex, uniqueCount, cornerTangents, remap);
    weldStream(uv0, firstVertex, uniqueCount, cornerUv0, remap);
    weldStream(uv1, firstVertex, uniqueCount, cornerUv1, remap);
    weldStream(colors, firstVertex, uniqueCount, cornerColors, remap);
    vertices.resize(positions.size());

    localIndices.assign(remap.begin(), remap.end());
    return static_cast<uint32_t>(uniqueCount);
}

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
    vertices.reserve(vertexTarget);
    indices.reserve(indices.size() + extraIndices);
}

glm::vec4 GeometryStore::boundingSphere(uint32_t firstVertex, uint32_t vertexCount) const
{
    if (vertexCount == 0) {
        return glm::vec4{0.0f};
    }
    const meshopt_Bounds bounds =
        meshopt_computeSphereBounds(&positions[firstVertex].x, vertexCount, sizeof(glm::vec3), nullptr, 0);
    return glm::vec4{bounds.center[0], bounds.center[1], bounds.center[2], bounds.radius};
}

BoundingBox GeometryStore::boundingBox(uint32_t firstVertex, uint32_t vertexCount) const
{
    if (vertexCount == 0) {
        return {};
    }
    BoundingBox box{.bboxMin = positions[firstVertex], .bboxMax = positions[firstVertex]};
    for (uint32_t v = firstVertex + 1; v < firstVertex + vertexCount; ++v) {
        box.bboxMin = glm::min(box.bboxMin, positions[v]);
        box.bboxMax = glm::max(box.bboxMax, positions[v]);
    }
    return box;
}

glm::vec4 GeometryStore::nodeBoundingSphere(uint32_t firstNode, uint32_t nodeCount) const
{
    if (nodeCount == 0) {
        return glm::vec4{0.0f};
    }
    // centers and radii are read in place from the NodeDraw rows
    const glm::vec4& firstSphere = nodeDraws[firstNode].boundingSphere;
    const meshopt_Bounds bounds =
        meshopt_computeSphereBounds(&firstSphere.x, nodeCount, sizeof(NodeDraw), &firstSphere.w, sizeof(NodeDraw));
    return glm::vec4{bounds.center[0], bounds.center[1], bounds.center[2], bounds.radius};
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

    // the Flex bound uses min_triangles: meshlets may be that small
    const size_t maxMeshlets = meshopt_buildMeshletsBound(indexCount, kMeshletMaxVertices, kMeshletMinTriangles);
    std::vector<meshopt_Meshlet> built(maxMeshlets);
    std::vector<unsigned int> localVertices(indexCount);
    std::vector<unsigned char> localTriangles(indexCount);

    const size_t meshletCount = meshopt_buildMeshletsFlex(
        built.data(), localVertices.data(), localTriangles.data(), rangeIndices.data(), indexCount, rangePositions,
        vertexCount, sizeof(GpuVertex), kMeshletMaxVertices, kMeshletMinTriangles, kMeshletMaxTriangles,
        kMeshletConeWeight, kMeshletSplitFactor);

    if (meshletCount == 0) {
        return {};
    }

    const meshopt_Meshlet& last = built[meshletCount - 1];
    localVertices.resize(last.vertex_offset + last.vertex_count);
    localTriangles.resize(last.triangle_offset + last.triangle_count * 3);

    MeshletBuild build{.meshlets = {},
                       .bounds = {},
                       .groups = {},
                       .vertices = std::move(localVertices),
                       .triangles = std::move(localTriangles)};
    std::vector<GpuMeshletDesc> descs;
    std::vector<GpuMeshletBounds> boxes;
    descs.reserve(meshletCount);
    boxes.reserve(meshletCount);

    for (size_t i = 0; i < meshletCount; ++i) {
        const meshopt_Meshlet& m = built[i];
        meshopt_optimizeMeshletLevel(build.vertices.data() + m.vertex_offset, m.vertex_count,
                                     build.triangles.data() + m.triangle_offset, m.triangle_count,
                                     kMeshletOptimizeLevel);

        const meshopt_Bounds bounds = meshopt_computeMeshletBounds(
            build.vertices.data() + m.vertex_offset, build.triangles.data() + m.triangle_offset, m.triangle_count,
            rangePositions, vertexCount, sizeof(GpuVertex));

        descs.push_back(GpuMeshletDesc{
            .vertexOffset = m.vertex_offset,
            .triangleOffset = m.triangle_offset,
            .vertexCount = static_cast<uint8_t>(m.vertex_count),
            .triangleCount = static_cast<uint8_t>(m.triangle_count),
            .coneAxis = {bounds.cone_axis_s8[0], bounds.cone_axis_s8[1], bounds.cone_axis_s8[2]},
            .coneCutoff = bounds.cone_cutoff_s8,
            .boundingSphere = glm::vec4{bounds.center[0], bounds.center[1], bounds.center[2], bounds.radius},
        });
        boxes.push_back(computeMeshletBounds(build.vertices.data() + m.vertex_offset,
                                             build.triangles.data() + m.triangle_offset, m.triangle_count,
                                             &vertices[firstVertex]));
    }

    // groups take contiguous meshlet runs; only the descriptors move, the vertex and triangle data stay put
    const MeshletPartition partition =
        partitionMeshlets(descs, build.vertices, build.triangles, rangePositions, vertexCount);
    build.meshlets.reserve(meshletCount);
    build.bounds.reserve(meshletCount);
    for (const uint32_t meshlet : partition.order) {
        build.meshlets.push_back(descs[meshlet]);
        build.bounds.push_back(boxes[meshlet]);
    }
    build.groups.reserve(partition.offsets.size() - 1);
    for (size_t g = 0; g + 1 < partition.offsets.size(); ++g) {
        GpuMeshletGroup group{
            .bboxMin = glm::vec3{std::numeric_limits<float>::max()},
            .firstMeshlet = partition.offsets[g],
            .bboxMax = glm::vec3{std::numeric_limits<float>::lowest()},
            .meshletCount = partition.offsets[g + 1] - partition.offsets[g],
        };
        for (uint32_t m = group.firstMeshlet; m < group.firstMeshlet + group.meshletCount; ++m) {
            group.bboxMin = glm::min(group.bboxMin, build.bounds[m].bboxMin);
            group.bboxMax = glm::max(group.bboxMax, build.bounds[m].bboxMax);
        }
        build.groups.push_back(group);
    }

    // rebase range-local vertex ids to scratch-absolute after optimize/bounds/partition used them
    for (uint32_t& vertex : build.vertices) {
        vertex += firstVertex;
    }

    logDebug(std::format("Built {} meshlets in {} groups for index range [{}, {}) ({} meshlet verts, {} local tri "
                         "corners)",
                         build.meshlets.size(), build.groups.size(), firstIndex, firstIndex + indexCount,
                         build.vertices.size(), build.triangles.size()),
             "AssetLoader");
    return build;
}

void GeometryStore::appendMeshlets(MeshletBuild&& build, PrimitiveDraw& draw)
{
    draw.meshlets = {};
    draw.firstGroup = static_cast<uint32_t>(meshletGroups.size());
    draw.groupCount = 0;
    if (build.meshlets.empty()) {
        return;
    }
    const uint32_t baseVertexOffset = static_cast<uint32_t>(meshletVertices.size());
    const uint32_t baseTriangleOffset = static_cast<uint32_t>(meshletTriangles.size());
    draw.meshlets = MeshletDraw{
        .firstMeshlet = static_cast<uint32_t>(meshlets.size()),
        .meshletCount = static_cast<uint32_t>(build.meshlets.size()),
    };
    draw.groupCount = static_cast<uint32_t>(build.groups.size());

    meshletVertices.insert(meshletVertices.end(), build.vertices.begin(), build.vertices.end());
    meshletTriangles.insert(meshletTriangles.end(), build.triangles.begin(), build.triangles.end());
    for (GpuMeshletDesc& meshlet : build.meshlets) {
        meshlet.vertexOffset += baseVertexOffset;
        meshlet.triangleOffset += baseTriangleOffset;
    }
    meshlets.insert(meshlets.end(), build.meshlets.begin(), build.meshlets.end());
    meshletBounds.insert(meshletBounds.end(), build.bounds.begin(), build.bounds.end());
    meshletGroups.insert(meshletGroups.end(), build.groups.begin(), build.groups.end());
}

void GeometryStore::buildMeshletsForRange(uint32_t firstIndex, uint32_t indexCount, uint32_t firstVertex,
                                          uint32_t vertexCount, PrimitiveDraw& draw)
{
    appendMeshlets(buildMeshlets(firstIndex, indexCount, firstVertex, vertexCount), draw);
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
    drop(indices);
    drop(vertices);
    drop(meshlets);
    drop(meshletBounds);
    // meshletGroups is catalog: the cull templates read it after the upload
    drop(meshletVertices);
    drop(meshletTriangles);
    drop(ommBakes);
    presentStreams = 0;
    textureRefs = {};
    ommSummary = {};
    flushedPrimitiveCount = static_cast<uint32_t>(primitiveDraws.size());
    flushedAuxBlobCount = static_cast<uint32_t>(auxBlobs.size());
    flushedNodeCount = static_cast<uint32_t>(nodeDraws.size());
    flushedGroupCount = static_cast<uint32_t>(meshletGroups.size());
}

void GeometryStore::discardScratch()
{
    primitiveDraws.resize(flushedPrimitiveCount);
    auxBlobs.resize(flushedAuxBlobCount);
    nodeDraws.resize(flushedNodeCount);
    meshletGroups.resize(flushedGroupCount);
    clearScratch();
}
