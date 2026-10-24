#ifndef ENGINE_CORE_FRAME_SLANG
#define ENGINE_CORE_FRAME_SLANG

#include "types.slang"
#include "core/binding.slang"

// everything a draw needs, resolved from the push data through the frame root
struct DrawContext
{
    GpuFrameRoot root;
    GpuDrawRecord record;
    GpuNodeInstance nodeInstance;
    GpuModel model;
    GpuPrimitive primitive;
};

DrawContext loadDrawContext(MeshPushData push)
{
    DrawContext draw;
    draw.root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    draw.record = bdaPtr<GpuDrawRecord>(draw.root.drawRecords)[push.drawIndex];
    draw.nodeInstance = bdaPtr<GpuNodeInstance>(draw.root.nodeInstances)[draw.record.nodeInstanceIndex];
    draw.model = bdaPtr<GpuModel>(draw.root.models)[draw.nodeInstance.modelIndex];
    draw.primitive = bdaPtr<GpuPrimitive>(draw.root.primitives)[draw.record.primitiveIndex];
    return draw;
}

GpuCameraData loadCamera(GpuFrameRoot root)
{
    return bdaPtr<GpuCameraData>(root.camera)[0];
}

// material rows are shared by all models, so the id is absolute
uint materialRow(DrawContext draw)
{
    return draw.primitive.materialId == kNoneIndex ? 0 : draw.primitive.materialId;
}

GpuMaterial loadMaterial(DrawContext draw)
{
    return bdaPtr<GpuMaterial>(draw.root.materials)[materialRow(draw)];
}

// parallel to loadMaterial: extension rows share the material row index
GpuMaterialExtension loadMaterialExtension(DrawContext draw)
{
    return bdaPtr<GpuMaterialExtension>(draw.root.pbrExtensions)[materialRow(draw)];
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
