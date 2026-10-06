#pragma once

#include "types.hpp"

#include <array>
#include <string>
#include <vector>

// One primitive's meshlets before they are appended to the store: offsets are local to this build,
// vertex ids are already scratch-absolute.
struct MeshletBuild
{
    std::vector<GpuMeshletDesc> meshlets;
    std::vector<uint32_t> vertices;
    std::vector<uint8_t> triangles;
};

class GeometryStore
{
public:
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec4> tangents;
    std::vector<glm::vec2> uv0;
    std::vector<glm::vec2> uv1;
    std::vector<glm::vec4> colors;
    std::vector<std::array<uint16_t, 4>> joints0;
    std::vector<glm::vec4> weights0;
    std::vector<uint32_t> indices;

    // packed {pos, color.rgb, uv0} for the current mesh shader
    std::vector<GpuVertex> vertices;

    std::vector<GpuMeshletDesc> meshlets;
    std::vector<uint32_t> meshletVertices;
    std::vector<uint8_t> meshletTriangles;
    std::vector<PrimitiveDraw> primitiveDraws;

    std::vector<MorphTarget> morphTargets;
    std::vector<glm::vec3> morphPos;
    std::vector<glm::vec3> morphNrm;
    std::vector<glm::vec4> morphTan;
    std::vector<float> morphWeights;

    std::vector<AuxBlob> auxBlobs;
    std::vector<std::string> extensionsUsed;
    std::vector<std::string> extensionsRequired;

    // catalog watermarks: primitiveDraws/morphTargets below these are GPU-absolute
    uint32_t flushedPrimitiveCount = 0;
    uint32_t flushedMorphTargetCount = 0;
    uint32_t flushedMorphWeightCount = 0;
    uint32_t flushedAuxBlobCount = 0;

    // grow SoA + packed GpuVertex to newCount; new verts get default attrs
    void resizeVertices(uint32_t newCount);
    // capacity for extraVertices more in all nine per-vertex arrays, and extraIndices more indices
    void reserveGeometry(size_t extraVertices, size_t extraIndices);

    // pack vertices[v] from positions/colors/uv0
    void packVertex(uint32_t v);

    // Pure: only reads indices/vertices, so ranges may be built concurrently.
    // indices in [firstIndex, +indexCount) must reference vertices in [firstVertex, +vertexCount)
    [[nodiscard]] MeshletBuild buildMeshlets(uint32_t firstIndex, uint32_t indexCount, uint32_t firstVertex,
                                             uint32_t vertexCount) const;
    // rebases a build onto the meshlet arrays; call in primitive order for deterministic offsets
    [[nodiscard]] MeshletDraw appendMeshlets(MeshletBuild&& build);
    [[nodiscard]] MeshletDraw buildMeshletsForRange(uint32_t firstIndex, uint32_t indexCount, uint32_t firstVertex,
                                                    uint32_t vertexCount);

    // drop load scratch after GPU append; catalog (primitiveDraws, morphTargets, weights, aux) stays
    void clearScratch();
    // failed load: catalog entries past the watermarks go too
    void discardScratch();
};
