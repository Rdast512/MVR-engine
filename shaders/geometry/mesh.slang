module mesh;

import types;
import core.binding;
import core.frame;

// meshlet data is model-relative: every table is indexed from the model's slice
public GpuMeshletDesc loadMeshlet(DrawContext draw)
{
    return bdaPtr<GpuMeshletDesc>(draw.model.meshlets)[draw.visible.meshletRow];
}

// model-relative vertex id of a meshlet corner
public uint loadMeshletVertexIndex(DrawContext draw, GpuMeshletDesc meshlet, uint vi)
{
    return bdaPtr<uint>(draw.model.meshletVertices)[meshlet.vertexOffset + vi];
}

public GpuVertex loadVertex(DrawContext draw, uint vertexIndex)
{
    return bdaPtr<GpuVertex>(draw.model.vertices)[vertexIndex];
}

public uint3 loadMeshletTriangle(DrawContext draw, GpuMeshletDesc meshlet, uint ti)
{
    uint8_t* corners = bdaPtr<uint8_t>(draw.model.meshletTriangles);
    uint base = meshlet.triangleOffset + ti * 3;
    return uint3(uint(corners[base + 0]), uint(corners[base + 1]), uint(corners[base + 2]));
}

// meshlet corner in model space
public GpuVertex loadMeshletCorner(DrawContext draw, GpuMeshletDesc meshlet, uint vi)
{
    return loadVertex(draw, loadMeshletVertexIndex(draw, meshlet, vi));
}

// a mirrored placement reverses winding; swapping two corners restores the front face
public uint3 loadFrontFacingTriangle(DrawContext draw, GpuMeshletDesc meshlet, uint ti)
{
    const uint3 tri = loadMeshletTriangle(draw, meshlet, ti);
    return (draw.nodeInstance.flags & kNodeFlagFlipWinding) != 0 ? tri.xzy : tri;
}
