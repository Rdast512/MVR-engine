#ifndef ENGINE_CORE_FRAME_SLANG
#define ENGINE_CORE_FRAME_SLANG

#include "types.slang"
#include "core/binding.slang"

// everything one visible meshlet needs, resolved from its render-list entry {node instance, meshlet} alone: the
// meshlet names its primitive, the primitive its material
struct DrawContext
{
    GpuFrameRoot root;
    GpuVisibleMeshlet visible;
    GpuNodeInstance nodeInstance;
    GpuModel model;
    uint primitiveRow; // row of the model's primitives slice
    GpuPrimitive primitive;
    bool isDoubleSided; // the material's, also DrawRecordFlag::TwoSided (MeshletCulling::drawRecordFlagsOf)
    uint visibleRow;    // row of root.visibleMeshlets; >= root.lateVisibleBase in the late phase
};

// visible: the {node instance, meshlet} a vis-buffer pixel or a ray hit decodes to; visibleRow is kNoneIndex for hits
DrawContext loadDrawContext(GpuFrameRoot root, GpuVisibleMeshlet visible, uint visibleRow)
{
    DrawContext draw;
    draw.root = root;
    draw.visibleRow = visibleRow;
    draw.visible = visible;
    draw.nodeInstance = bdaPtr<GpuNodeInstance>(draw.root.nodeInstances)[draw.visible.nodeInstanceIndex];
    draw.model = bdaPtr<GpuModel>(draw.root.models)[draw.nodeInstance.modelIndex];
    draw.primitiveRow = bdaPtr<uint>(draw.model.meshletPrimitives)[draw.visible.meshletRow];
    draw.primitive = bdaPtr<GpuPrimitive>(draw.model.primitives)[draw.primitiveRow];
    // materialRow below; kNoneIndex is the default material, row 0
    const uint material = draw.primitive.materialId == kNoneIndex ? 0 : draw.primitive.materialId;
    GpuMaterial* materialRef = bdaPtr<GpuMaterial>(draw.root.materials) + material;
    draw.isDoubleSided = (materialRef.flags & kMaterialFlagDoubleSided) != 0;
    return draw;
}

// a row of root.visibleMeshlets: a vis-buffer payload's or a mesh workgroup's
DrawContext loadDrawContext(GpuFrameRoot root, uint visibleRow)
{
    return loadDrawContext(root, bdaPtr<GpuVisibleMeshlet>(root.visibleMeshlets)[visibleRow], visibleRow);
}

// groupId: mesh workgroup within the sequence's batch
DrawContext loadDrawContext(MeshPushData push, uint groupId)
{
    return loadDrawContext(bdaPtr<GpuFrameRoot>(push.frameRoot)[0], push.firstVisible + groupId);
}

GpuTargets loadTargets(GpuFrameRoot root)
{
    return bdaPtr<GpuTargets>(root.targets)[0];
}

GpuCameraData loadCamera(GpuFrameRoot root)
{
    return bdaPtr<GpuCameraData>(root.camera)[0];
}

// material rows are shared by all models, so the id is absolute
uint materialRow(GpuPrimitive primitive)
{
    return primitive.materialId == kNoneIndex ? 0 : primitive.materialId;
}

GpuMaterial loadMaterial(GpuFrameRoot root, uint row)
{
    return bdaPtr<GpuMaterial>(root.materials)[row];
}

// parallel to loadMaterial: extension rows share the material row index
GpuMaterialExtension loadMaterialExtension(GpuFrameRoot root, uint row)
{
    return bdaPtr<GpuMaterialExtension>(root.pbrExtensions)[row];
}

// lights are stored in model space; the owning instance places them in the world
GpuLight loadWorldLight(GpuFrameRoot root, uint lightIndex)
{
    GpuLight light = bdaPtr<GpuLight>(root.lights)[lightIndex];
    const float4x4 model = bdaPtr<GpuObjectUB>(root.instances)[light.instanceIndex].modelMatrix;
    light.position = mul(model, float4(light.position, 1.0)).xyz;
    light.direction = normalize(mul(model, float4(light.direction, 0.0)).xyz);
    return light;
}

#endif
