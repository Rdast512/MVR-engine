#include "geometry_store.hpp"

#include "../static_headers/logger.hpp"
#include "../util/vk_tracy.hpp"

#include <meshoptimizer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>

namespace
{
    // m8 MG-P3: Spatial builder for tighter CLAS/BVH boxes, as clusterlod.h's clodDefaultConfigRT (vk_lod_clusters RT
    // preset). Its min = max / 4 and fill weight 0.5 inflated Bistro's clusters; min = max / 3 and 0.75 measured faster
    constexpr size_t kMeshletMaxVertices = 64;
    constexpr size_t kMeshletMaxTriangles = 126;
    constexpr size_t kMeshletMinTriangles = kMeshletMaxTriangles / 3;
    static_assert(kMeshletMaxVertices <= UINT8_MAX && kMeshletMaxTriangles <= UINT8_MAX, "GpuMeshletDesc stores 8-bit counts");
    constexpr float kMeshletFillWeight = 0.75f;
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

    uint32_t packColorUnorm8(const glm::vec4& color)
    {
        const uint32_t r = static_cast<uint32_t>(meshopt_quantizeUnorm(color.r, 8));
        const uint32_t g = static_cast<uint32_t>(meshopt_quantizeUnorm(color.g, 8));
        const uint32_t b = static_cast<uint32_t>(meshopt_quantizeUnorm(color.b, 8));
        const uint32_t a = static_cast<uint32_t>(meshopt_quantizeUnorm(color.a, 8));
        return r | (g << 8) | (b << 16) | (a << 24);
    }

    uint32_t packHalf2(const glm::vec2& uv)
    {
        const uint32_t x = static_cast<uint32_t>(meshopt_quantizeHalf(uv.x));
        const uint32_t y = static_cast<uint32_t>(meshopt_quantizeHalf(uv.y));
        return x | (y << 16);
    }

    bool fitsHalf2(std::span<const glm::vec2> uvs, glm::vec2 texelScale)
    {
        const bool isTextured = texelScale.x > 0.0f || texelScale.y > 0.0f;
        constexpr float kMaxTexelError = 0.5f;
        constexpr float kMaxUntexturedError = 1e-3f;
        for (const glm::vec2& uv : uvs) {
            const float qu = meshopt_dequantizeHalf(meshopt_quantizeHalf(uv.x));
            const float qv = meshopt_dequantizeHalf(meshopt_quantizeHalf(uv.y));
            if (!std::isfinite(qu) || !std::isfinite(qv)) {
                return false;
            }
            const float du = std::abs(qu - uv.x);
            const float dv = std::abs(qv - uv.y);
            if (isTextured) {
                if (du * texelScale.x > kMaxTexelError || dv * texelScale.y > kMaxTexelError) {
                    return false;
                }
            } else if (du > kMaxUntexturedError || dv > kMaxUntexturedError) {
                return false;
            }
        }
        return true;
    }

    bool isFiniteVec3(const glm::vec3& v)
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
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
    packed.color = packColorUnorm8(colors[v]);
    packed.texCoord = packHalf2(uv0[v]);
    vertices[v] = packed;
}

void GeometryStore::optimizeVertexFetch(uint32_t firstPrimitive)
{
    ZoneScopedN("GeometryStore::optimizeVertexFetch");
    const size_t primitiveCount = primitiveDraws.size() - firstPrimitive;
    if (primitiveCount == 0 || positions.empty()) {
        return;
    }

    const uint32_t oldTotalVerts = static_cast<uint32_t>(positions.size());
    const uint32_t baseVertex = primitiveDraws[firstPrimitive].firstVertex;

    std::vector<glm::vec3> newPositions;
    std::vector<glm::vec3> newNormals;
    std::vector<glm::vec4> newTangents;
    std::vector<glm::vec2> newUv0;
    std::vector<glm::vec2> newUv1;
    std::vector<glm::vec4> newColors;
    std::vector<GpuVertex> newVertices;

    newPositions.reserve(oldTotalVerts);
    newNormals.reserve(oldTotalVerts);
    newTangents.reserve(oldTotalVerts);
    newUv0.reserve(oldTotalVerts);
    newUv1.reserve(oldTotalVerts);
    newColors.reserve(oldTotalVerts);
    newVertices.reserve(oldTotalVerts);

    if (baseVertex > 0) {
        newPositions.insert(newPositions.end(), positions.begin(), positions.begin() + baseVertex);
        newNormals.insert(newNormals.end(), normals.begin(), normals.begin() + baseVertex);
        newTangents.insert(newTangents.end(), tangents.begin(), tangents.begin() + baseVertex);
        newUv0.insert(newUv0.end(), uv0.begin(), uv0.begin() + baseVertex);
        newUv1.insert(newUv1.end(), uv1.begin(), uv1.begin() + baseVertex);
        newColors.insert(newColors.end(), colors.begin(), colors.begin() + baseVertex);
        newVertices.insert(newVertices.end(), vertices.begin(), vertices.begin() + baseVertex);
    }

    std::vector<unsigned int> localMeshletVerts;
    std::vector<unsigned int> remap;

    const auto appendRemapped = [&](auto& dst, const auto& src, uint32_t oldFirst, uint32_t oldCount,
                                    uint32_t newFirst, size_t uniqueCount) {
        dst.resize(newFirst + uniqueCount);
        if (uniqueCount > 0) {
            meshopt_remapVertexBuffer(dst.data() + newFirst, src.data() + oldFirst, oldCount, sizeof(dst[0]),
                                      remap.data());
        }
    };

    for (uint32_t p = firstPrimitive; p < primitiveDraws.size(); ++p) {
        PrimitiveDraw& draw = primitiveDraws[p];
        const uint32_t oldFirstVertex = draw.firstVertex;
        const uint32_t oldVertexCount = draw.vertexCount;
        const uint32_t newFirstVertex = static_cast<uint32_t>(newPositions.size());

        if (oldVertexCount == 0) {
            draw.firstVertex = newFirstVertex;
            continue;
        }

        if (draw.meshlets.meshletCount == 0) {
            newPositions.insert(newPositions.end(), positions.begin() + oldFirstVertex,
                                positions.begin() + oldFirstVertex + oldVertexCount);
            newNormals.insert(newNormals.end(), normals.begin() + oldFirstVertex,
                              normals.begin() + oldFirstVertex + oldVertexCount);
            newTangents.insert(newTangents.end(), tangents.begin() + oldFirstVertex,
                               tangents.begin() + oldFirstVertex + oldVertexCount);
            newUv0.insert(newUv0.end(), uv0.begin() + oldFirstVertex, uv0.begin() + oldFirstVertex + oldVertexCount);
            newUv1.insert(newUv1.end(), uv1.begin() + oldFirstVertex, uv1.begin() + oldFirstVertex + oldVertexCount);
            newColors.insert(newColors.end(), colors.begin() + oldFirstVertex,
                             colors.begin() + oldFirstVertex + oldVertexCount);
            newVertices.insert(newVertices.end(), vertices.begin() + oldFirstVertex,
                               vertices.begin() + oldFirstVertex + oldVertexCount);
            const int64_t delta = static_cast<int64_t>(newFirstVertex) - static_cast<int64_t>(oldFirstVertex);
            for (uint32_t i = draw.firstIndex; i < draw.firstIndex + draw.indexCount; ++i) {
                indices[i] = static_cast<uint32_t>(static_cast<int64_t>(indices[i]) + delta);
            }
            draw.firstVertex = newFirstVertex;
            continue;
        }

        // visit vertices in final partitioned meshlet order so sequential meshlets stream contiguous vertex runs
        localMeshletVerts.clear();
        for (uint32_t m = draw.meshlets.firstMeshlet; m < draw.meshlets.firstMeshlet + draw.meshlets.meshletCount;
             ++m) {
            const GpuMeshletDesc& meshlet = meshlets[m];
            for (uint32_t vi = 0; vi < meshlet.vertexCount; ++vi) {
                localMeshletVerts.push_back(meshletVertices[meshlet.vertexOffset + vi] - oldFirstVertex);
            }
        }

        remap.resize(oldVertexCount);
        const size_t uniqueCount = meshopt_optimizeVertexFetchRemap(remap.data(), localMeshletVerts.data(),
                                                                    localMeshletVerts.size(), oldVertexCount);

        appendRemapped(newPositions, positions, oldFirstVertex, oldVertexCount, newFirstVertex, uniqueCount);
        appendRemapped(newNormals, normals, oldFirstVertex, oldVertexCount, newFirstVertex, uniqueCount);
        appendRemapped(newTangents, tangents, oldFirstVertex, oldVertexCount, newFirstVertex, uniqueCount);
        appendRemapped(newUv0, uv0, oldFirstVertex, oldVertexCount, newFirstVertex, uniqueCount);
        appendRemapped(newUv1, uv1, oldFirstVertex, oldVertexCount, newFirstVertex, uniqueCount);
        appendRemapped(newColors, colors, oldFirstVertex, oldVertexCount, newFirstVertex, uniqueCount);
        appendRemapped(newVertices, vertices, oldFirstVertex, oldVertexCount, newFirstVertex, uniqueCount);

        for (uint32_t m = draw.meshlets.firstMeshlet; m < draw.meshlets.firstMeshlet + draw.meshlets.meshletCount;
             ++m) {
            const GpuMeshletDesc& meshlet = meshlets[m];
            for (uint32_t vi = 0; vi < meshlet.vertexCount; ++vi) {
                uint32_t& v = meshletVertices[meshlet.vertexOffset + vi];
                v = newFirstVertex + remap[v - oldFirstVertex];
            }
        }
        for (uint32_t i = draw.firstIndex; i < draw.firstIndex + draw.indexCount; ++i) {
            const uint32_t mapped = remap[indices[i] - oldFirstVertex];
            indices[i] = mapped != ~0u ? newFirstVertex + mapped : newFirstVertex;
        }

        draw.firstVertex = newFirstVertex;
        draw.vertexCount = static_cast<uint32_t>(uniqueCount);
    }

    const uint32_t newTotalVerts = static_cast<uint32_t>(newPositions.size());
    positions = std::move(newPositions);
    normals = std::move(newNormals);
    tangents = std::move(newTangents);
    uv0 = std::move(newUv0);
    uv1 = std::move(newUv1);
    colors = std::move(newColors);
    vertices = std::move(newVertices);

    logInfo(std::format("glTF vertex fetch reorder: {} -> {} verts (-{} unused) across {} primitives", oldTotalVerts,
                        newTotalVerts, oldTotalVerts - newTotalVerts, primitiveCount),
            "AssetLoader");
}

void GeometryStore::packAttributes(uint32_t firstPrimitive, std::span<const PrimitiveUvPrecision> uvPrecisions)
{
    ZoneScopedN("GeometryStore::packAttributes");
    const size_t vertexCount = positions.size();
    const size_t primitiveCount = primitiveDraws.size() - firstPrimitive;
    uvFloat.clear();

    uint32_t halfUv0Prims = 0;
    uint32_t floatUv0Prims = 0;
    uint32_t halfUv1Prims = 0;
    uint32_t floatUv1Prims = 0;
    const bool hasUv1 = (presentStreams & GpuModelAttribute::Uv1) != 0;

    for (size_t i = 0; i < primitiveCount; ++i) {
        PrimitiveDraw& draw = primitiveDraws[firstPrimitive + i];
        const PrimitiveUvPrecision prec = i < uvPrecisions.size() ? uvPrecisions[i] : PrimitiveUvPrecision{};
        draw.uv0FloatOffset = kNoneIndex;
        draw.uv1FloatOffset = kNoneIndex;
        if (draw.vertexCount == 0) {
            continue;
        }

        const auto uv0Slice = std::span(uv0).subspan(draw.firstVertex, draw.vertexCount);
        if (fitsHalf2(uv0Slice, prec.uv0TexelScale)) {
            // overwrite CPU uv0 with the exact quantized value so OMM baking & emissive culling match GPU bit-for-bit
            for (glm::vec2& uv : uv0Slice) {
                uv.x = meshopt_dequantizeHalf(meshopt_quantizeHalf(uv.x));
                uv.y = meshopt_dequantizeHalf(meshopt_quantizeHalf(uv.y));
            }
            ++halfUv0Prims;
        } else {
            draw.uv0FloatOffset = static_cast<uint32_t>(uvFloat.size());
            uvFloat.insert(uvFloat.end(), uv0Slice.begin(), uv0Slice.end());
            ++floatUv0Prims;
        }

        if (hasUv1) {
            const auto uv1Slice = std::span(uv1).subspan(draw.firstVertex, draw.vertexCount);
            if (fitsHalf2(uv1Slice, prec.uv1TexelScale)) {
                for (glm::vec2& uv : uv1Slice) {
                    uv.x = meshopt_dequantizeHalf(meshopt_quantizeHalf(uv.x));
                    uv.y = meshopt_dequantizeHalf(meshopt_quantizeHalf(uv.y));
                }
                ++halfUv1Prims;
            } else {
                draw.uv1FloatOffset = static_cast<uint32_t>(uvFloat.size());
                uvFloat.insert(uvFloat.end(), uv1Slice.begin(), uv1Slice.end());
                ++floatUv1Prims;
            }
        }
    }

    if (!uvFloat.empty()) {
        presentStreams |= GpuModelAttribute::UvFloat;
    }

    vertices.resize(vertexCount);
    for (uint32_t v = 0; v < vertexCount; ++v) {
        packVertex(v);
    }

    if ((presentStreams & GpuModelAttribute::Normals) != 0 && vertexCount > 0) {
        std::vector<glm::vec4> src(vertexCount, glm::vec4{0.0f});
        for (size_t v = 0; v < vertexCount; ++v) {
            const glm::vec3 n = normals[v];
            const float lenSq = glm::dot(n, n);
            if (lenSq > 0.0f && isFiniteVec3(n)) {
                src[v] = glm::vec4{n / std::sqrt(lenSq), 0.0f};
            }
        }
        std::vector<int16_t> oct(vertexCount * 4);
        meshopt_encodeFilterOct(oct.data(), vertexCount, sizeof(int16_t) * 4, 16, &src[0].x);
        packedNormals.resize(vertexCount);
        for (size_t v = 0; v < vertexCount; ++v) {
            const glm::vec3 n = normals[v];
            if (glm::dot(n, n) > 0.0f && isFiniteVec3(n)) {
                const uint32_t sx = static_cast<uint16_t>(oct[v * 4 + 0]);
                const uint32_t sy = static_cast<uint16_t>(oct[v * 4 + 1]);
                packedNormals[v] = sx | (sy << 16);
            } else {
                packedNormals[v] = kPackedOctZero;
            }
        }
    } else {
        packedNormals.clear();
    }

    if ((presentStreams & GpuModelAttribute::Tangents) != 0 && vertexCount > 0) {
        std::vector<glm::vec4> src(vertexCount, glm::vec4{0.0f});
        for (size_t v = 0; v < vertexCount; ++v) {
            const glm::vec4 t = tangents[v];
            const glm::vec3 xyz{t.x, t.y, t.z};
            const float lenSq = glm::dot(xyz, xyz);
            if (t.w != 0.0f && lenSq > 0.0f && isFiniteVec3(xyz) && std::isfinite(t.w)) {
                src[v] = glm::vec4{xyz / std::sqrt(lenSq), 0.0f};
            }
        }
        std::vector<int16_t> oct(vertexCount * 4);
        meshopt_encodeFilterOct(oct.data(), vertexCount, sizeof(int16_t) * 4, 15, &src[0].x);
        packedTangents.resize(vertexCount);
        for (size_t v = 0; v < vertexCount; ++v) {
            const glm::vec4 t = tangents[v];
            const glm::vec3 xyz{t.x, t.y, t.z};
            if (t.w != 0.0f && glm::dot(xyz, xyz) > 0.0f && isFiniteVec3(xyz) && std::isfinite(t.w)) {
                const uint32_t sx = static_cast<uint32_t>(oct[v * 4 + 0]) & 0x7FFFu;
                const uint32_t sy = static_cast<uint32_t>(oct[v * 4 + 1]) & 0x7FFFu;
                const uint32_t signBit = t.w < 0.0f ? 0x40000000u : 0u;
                packedTangents[v] = 0x80000000u | signBit | (sy << 15) | sx;
            } else {
                packedTangents[v] = 0u;
            }
        }
    } else {
        packedTangents.clear();
    }

    if (hasUv1 && vertexCount > 0) {
        packedUv1.resize(vertexCount);
        for (size_t v = 0; v < vertexCount; ++v) {
            packedUv1[v] = packHalf2(uv1[v]);
        }
    } else {
        packedUv1.clear();
    }

    logInfo(std::format("glTF attribute packing: {} verts | UV0 half2={} fp32={} | UV1 half2={} fp32={} | uvFloat={} verts",
                        vertexCount, halfUv0Prims, floatUv0Prims, halfUv1Prims, floatUv1Prims, uvFloat.size()),
            "AssetLoader");
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

    // the Spatial bound uses min_triangles: meshlets may be that small
    const size_t maxMeshlets = meshopt_buildMeshletsBound(indexCount, kMeshletMaxVertices, kMeshletMinTriangles);
    std::vector<meshopt_Meshlet> built(maxMeshlets);
    std::vector<unsigned int> localVertices(indexCount);
    std::vector<unsigned char> localTriangles(indexCount);

    const size_t meshletCount = meshopt_buildMeshletsSpatial(
        built.data(), localVertices.data(), localTriangles.data(), rangeIndices.data(), indexCount, rangePositions,
        vertexCount, sizeof(GpuVertex), kMeshletMaxVertices, kMeshletMinTriangles, kMeshletMaxTriangles,
        kMeshletFillWeight);

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

    // groups take contiguous meshlet runs. The triangles follow their meshlets, so the primitive's triangles are dense
    // in meshlet order and a triangle-light row maps to its meshlet by triangleOffset (spec/m7/e2_emissive_triangles.md);
    // the vertex data stays put
    const MeshletPartition partition =
        partitionMeshlets(descs, build.vertices, build.triangles, rangePositions, vertexCount);
    build.meshlets.reserve(meshletCount);
    build.bounds.reserve(meshletCount);
    std::vector<uint8_t> orderedTriangles;
    orderedTriangles.reserve(build.triangles.size());
    for (const uint32_t meshlet : partition.order) {
        GpuMeshletDesc desc = descs[meshlet];
        const auto first = build.triangles.begin() + desc.triangleOffset;
        desc.triangleOffset = static_cast<uint32_t>(orderedTriangles.size());
        orderedTriangles.insert(orderedTriangles.end(), first, first + desc.triangleCount * 3);
        build.meshlets.push_back(desc);
        build.bounds.push_back(boxes[meshlet]);
    }
    build.triangles = std::move(orderedTriangles);
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
        .triangleCount = static_cast<uint32_t>(build.triangles.size() / 3),
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
    drop(packedNormals);
    drop(packedTangents);
    drop(packedUv1);
    drop(uvFloat);
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
    flushedEmissiveActiveTriCount = static_cast<uint32_t>(emissiveActiveTriangles.size());
}

void GeometryStore::discardScratch()
{
    primitiveDraws.resize(flushedPrimitiveCount);
    auxBlobs.resize(flushedAuxBlobCount);
    nodeDraws.resize(flushedNodeCount);
    meshletGroups.resize(flushedGroupCount);
    emissiveActiveTriangles.resize(flushedEmissiveActiveTriCount);
    clearScratch();
}
