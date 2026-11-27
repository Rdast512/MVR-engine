#pragma once

#include "omm_baker.hpp"
#include "types.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// One primitive's meshlets before they are appended to the store: offsets are local to this build,
// vertex ids are already scratch-absolute.
struct MeshletBuild
{
    std::vector<GpuMeshletDesc> meshlets;
    std::vector<GpuMeshletBounds> bounds; // parallel to meshlets
    std::vector<GpuMeshletGroup> groups;  // firstMeshlet relative to this build
    std::vector<uint32_t> vertices;
    std::vector<uint8_t> triangles;
};

// streams one primitive lacks and must get generated
struct ShadingAttributeRequest
{
    bool needsNormals = false;
    bool needsTangents = false;
    // UV set of the normal texture, 0 or 1
    uint32_t tangentUvSet = 0;
};

// max texel error scale per UV set (max(texDim * uvMatrixLinearScale) across material textures)
struct PrimitiveUvPrecision
{
    glm::vec2 uv0TexelScale{0.0f};
    glm::vec2 uv1TexelScale{0.0f};
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
    std::vector<uint32_t> indices;

    // packed GPU streams built by packAttributes (engine spec §6.3)
    std::vector<GpuVertex> vertices;
    std::vector<uint32_t> packedNormals;
    std::vector<uint32_t> packedTangents;
    std::vector<uint32_t> packedUv1;
    // compact per-primitive FP32 UV fallback for primitives where half2 exceeds the texel error budget
    std::vector<glm::vec2> uvFloat;

    std::vector<GpuMeshletDesc> meshlets;
    std::vector<GpuMeshletBounds> meshletBounds; // parallel to meshlets
    std::vector<uint32_t> meshletVertices;
    std::vector<uint8_t> meshletTriangles;
    std::vector<PrimitiveDraw> primitiveDraws;
    // catalog, kept after the upload: the cull templates build one draw record per group
    std::vector<GpuMeshletGroup> meshletGroups;
    // catalog, kept after the upload: active primitive-local meshlet triangle indices for partial textured emitters
    // (m8 I14 F27); indexed by PrimitiveDraw::{firstEmissiveTriangle, emissiveTriangleCount}
    std::vector<uint32_t> emissiveActiveTriangles;
    // mesh placements; a mesh's primitives appear once in primitiveDraws however many nodes place it
    std::vector<NodeDraw> nodeDraws;

    std::vector<AuxBlob> auxBlobs;
    std::vector<std::string> extensionsUsed;
    std::vector<std::string> extensionsRequired;
    // texture heap indices the loaded model retains; the flush hands them to its ModelAllocation
    std::vector<uint32_t> textureRefs;
    // parallel to this load's primitiveDraws (from flushedPrimitiveCount); D2's micromap input
    std::vector<OmmBake> ommBakes;
    // the flush hands it to its ModelAllocation
    OmmBakeSummary ommSummary;

    // GpuModelAttribute bits of the optional streams any primitive of this load provided;
    // absent streams stay zero-filled in scratch and are not uploaded
    uint32_t presentStreams = 0;

    // catalog watermarks: primitiveDraws below these are GPU-absolute
    uint32_t flushedPrimitiveCount = 0;
    uint32_t flushedAuxBlobCount = 0;
    uint32_t flushedNodeCount = 0;
    uint32_t flushedGroupCount = 0;
    uint32_t flushedEmissiveActiveTriCount = 0;

    // grow SoA + packed GpuVertex to newCount; new verts get default attrs
    void resizeVertices(uint32_t newCount);
    // capacity for extraVertices more in all seven per-vertex arrays, and extraIndices more indices
    void reserveGeometry(size_t extraVertices, size_t extraIndices);

    // enclosing sphere of positions [firstVertex, +vertexCount); zero when empty
    [[nodiscard]] glm::vec4 boundingSphere(uint32_t firstVertex, uint32_t vertexCount) const;
    // box of positions [firstVertex, +vertexCount); zero when empty
    [[nodiscard]] BoundingBox boundingBox(uint32_t firstVertex, uint32_t vertexCount) const;
    // enclosing sphere of the node spheres [firstNode, +nodeCount); zero when empty
    [[nodiscard]] glm::vec4 nodeBoundingSphere(uint32_t firstNode, uint32_t nodeCount) const;

    // pack vertices[v] from positions/colors/uv0 (provisional pre-meshlet pack; packAttributes writes final packed GPU streams)
    void packVertex(uint32_t v);

    // Reorder each primitive's vertices in final partitioned meshlet order (meshopt_optimizeVertexFetchRemap), compacting
    // unreferenced vertices within each primitive and remapping meshletVertices, indices, and all CPU vertex streams.
    // Call after buildGltfMeshlets and before packAttributes / bakeGltfOmms / emissive culling.
    void optimizeVertexFetch(uint32_t firstPrimitive);

    // Pack GPU vertex streams (GpuVertex 20 B, octahedral snorm16 normals, octahedral snorm15+sign tangents, half2 UV0/UV1
    // with compact per-primitive FP32 fallback in uvFloat). Writes quantized UVs back to CPU uv0/uv1 for half2 primitives
    // so OMM baking and emissive culling see bit-identical UVs to raster/rays.
    void packAttributes(uint32_t firstPrimitive, std::span<const PrimitiveUvPrecision> uvPrecisions);

    // generates the requested streams for the last primitive's vertices [firstVertex, end). Vertices split on seams
    // and unused ones drop, so localIndices are rewritten; returns the new vertex count. Call before packVertex
    [[nodiscard]] uint32_t generateShadingAttributes(uint32_t firstVertex, std::vector<uint32_t>& localIndices,
                                                     const ShadingAttributeRequest& request);

    // Pure: only reads indices/vertices, so ranges may be built concurrently.
    // indices in [firstIndex, +indexCount) must reference vertices in [firstVertex, +vertexCount)
    [[nodiscard]] MeshletBuild buildMeshlets(uint32_t firstIndex, uint32_t indexCount, uint32_t firstVertex,
                                             uint32_t vertexCount) const;
    // rebases a build onto the meshlet arrays and sets draw's meshlet and group ranges; call in primitive order for
    // deterministic offsets
    void appendMeshlets(MeshletBuild&& build, PrimitiveDraw& draw);
    void buildMeshletsForRange(uint32_t firstIndex, uint32_t indexCount, uint32_t firstVertex, uint32_t vertexCount,
                               PrimitiveDraw& draw);

    // drop load scratch after GPU append; catalog (primitiveDraws, nodeDraws, meshletGroups, aux) stays
    void clearScratch();
    // failed load: catalog entries past the watermarks go too
    void discardScratch();
};
