#pragma once

#include "types.hpp"

#include <array>
#include <string>
#include <vector>

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

    // grow SoA + packed GpuVertex to newCount; new verts get default attrs
    void resizeVertices(uint32_t newCount);

    // pack vertices[v] from positions/colors/uv0
    void packVertex(uint32_t v);

    [[nodiscard]] MeshletDraw buildMeshletsForRange(uint32_t firstIndex, uint32_t indexCount);
};
