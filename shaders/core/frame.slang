module frame;

import types;
import core.binding;

// everything one visible meshlet needs, resolved from its render-list entry {node instance, meshlet} alone: the
// meshlet names its primitive, the primitive its material
public struct DrawContext
{
    public GpuFrameRoot root;
    public GpuVisibleMeshlet visible;
    public GpuNodeInstance nodeInstance;
    public GpuModel model;
    public uint primitiveRow; // row of the model's primitives slice; kNoneIndex for hits
    public GpuPrimitive primitive;
    // the material's, copied into the primitive at load; also DrawRecordFlag::TwoSided
    public bool isDoubleSided;
    public uint visibleRow;    // row of root.visibleMeshlets; >= root.lateVisibleBase in the late phase
};

// visible: the {node instance, meshlet} a vis-buffer pixel or a ray hit decodes to; visibleRow is kNoneIndex for hits
public DrawContext loadDrawContext(GpuFrameRoot root, GpuVisibleMeshlet visible, uint visibleRow)
{
    DrawContext draw;
    draw.root = root;
    draw.visibleRow = visibleRow;
    draw.visible = visible;
    draw.nodeInstance = bdaPtr<GpuNodeInstance>(draw.root.nodeInstances)[draw.visible.nodeInstanceIndex];
    draw.model = bdaPtr<GpuModel>(draw.root.models)[draw.nodeInstance.modelIndex];
    draw.primitiveRow = bdaPtr<uint>(draw.model.meshletPrimitives)[draw.visible.meshletRow];
    draw.primitive = bdaPtr<GpuPrimitive>(draw.model.primitives)[draw.primitiveRow];
    draw.isDoubleSided = (draw.primitive.flags & kPrimitiveFlagDoubleSided) != 0;
    return draw;
}

// a row of root.visibleMeshlets: a vis-buffer payload's or a mesh workgroup's
public DrawContext loadDrawContext(GpuFrameRoot root, uint visibleRow)
{
    return loadDrawContext(root, bdaPtr<GpuVisibleMeshlet>(root.visibleMeshlets)[visibleRow], visibleRow);
}

// ray-hit draw context (m8 MG-P4): reads only modelIndex/flags from GpuNodeInstance (skipping 128 B of world/prev
// matrices already carried by RayHit::objectToWorld) and looks up GpuPrimitive by the absolute
// GpuRayInstance::primitiveRow (skipping model.meshletPrimitives)
public DrawContext loadRayDrawContext(GpuFrameRoot root, uint rayInstance, uint meshletRow, float3x4 objectToWorld)
{
    const GpuRayInstance ray = bdaPtr<GpuRayInstance>(root.rayInstances)[rayInstance];
    GpuNodeInstance* nodePtr = bdaPtr<GpuNodeInstance>(root.nodeInstances) + ray.nodeInstanceIndex;
    const uint modelIndex = nodePtr.modelIndex;
    const uint nodeFlags = nodePtr.flags;

    DrawContext draw;
    draw.root = root;
    draw.visibleRow = kNoneIndex;
    draw.visible.nodeInstanceIndex = ray.nodeInstanceIndex;
    draw.visible.meshletRow = meshletRow;
    draw.nodeInstance.worldMatrix =
        float4x4(objectToWorld[0], objectToWorld[1], objectToWorld[2], float4(0.0, 0.0, 0.0, 1.0));
    draw.nodeInstance.prevWorldMatrix =
        float4x4(float4(1.0, 0.0, 0.0, 0.0), float4(0.0, 1.0, 0.0, 0.0), float4(0.0, 0.0, 1.0, 0.0),
                 float4(0.0, 0.0, 0.0, 1.0));
    draw.nodeInstance.entityIndex = 0;
    draw.nodeInstance.modelIndex = modelIndex;
    draw.nodeInstance.nodeIndex = 0;
    draw.nodeInstance.flags = nodeFlags;
    draw.model = bdaPtr<GpuModel>(root.models)[modelIndex];
    // the hit has the absolute row only, not the model-relative one
    draw.primitiveRow = kNoneIndex;
    draw.primitive = bdaPtr<GpuPrimitive>(root.primitives)[ray.primitiveRow];
    draw.isDoubleSided = (draw.primitive.flags & kPrimitiveFlagDoubleSided) != 0;
    return draw;
}

// groupId: mesh workgroup within the sequence's batch
public DrawContext loadDrawContext(MeshPushData push, uint groupId)
{
    return loadDrawContext(bdaPtr<GpuFrameRoot>(push.frameRoot)[0], push.firstVisible + groupId);
}

// false on a reset frame: last frame's images and rows do not apply
public bool hasHistory(GpuFrameRoot root)
{
    return (root.frameFlags & kFrameFlagHistoryReset) == 0;
}

// also false after an indirect mode or reference change: last frame's GI reservoirs hold other samples
public bool hasIndirectHistory(GpuFrameRoot root)
{
    return (root.frameFlags & (kFrameFlagHistoryReset | kFrameFlagIndirectHistoryReset)) == 0;
}

public GpuTargets loadTargets(GpuFrameRoot root)
{
    return bdaPtr<GpuTargets>(root.targets)[0];
}

public GpuCameraData loadCamera(GpuFrameRoot root)
{
    return bdaPtr<GpuCameraData>(root.camera)[0];
}

// material rows are shared by all models, so the id is absolute
public uint materialRow(GpuPrimitive primitive)
{
    return primitive.materialId == kNoneIndex ? 0 : primitive.materialId;
}

public GpuMaterial loadMaterial(GpuFrameRoot root, uint row)
{
    return bdaPtr<GpuMaterial>(root.materials)[row];
}

// parallel to loadMaterial: extension rows share the material row index
public GpuMaterialExtension loadMaterialExtension(GpuFrameRoot root, uint row)
{
    return bdaPtr<GpuMaterialExtension>(root.pbrExtensions)[row];
}

// a model-space light row placed by its owning instance: unit direction; intensity, range and cone keep their values,
// since KHR_lights_punctual does not scale them by the node
public GpuLight toWorldLight(GpuFrameRoot root, GpuLight light)
{
    const float4x4 model = bdaPtr<GpuObjectUB>(root.instances)[light.instanceIndex].modelMatrix;
    light.position = mul(model, float4(light.position, 1.0)).xyz;
    light.direction = normalize(mul(model, float4(light.direction, 0.0)).xyz);
    return light;
}

// this frame's world-space row (passes/light_world.slang); readers wait for the compute timeline
public GpuLight loadWorldLight(GpuFrameRoot root, uint lightIndex)
{
    return bdaPtr<GpuLight>(root.worldLights)[lightIndex];
}
