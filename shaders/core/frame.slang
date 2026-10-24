#ifndef ENGINE_CORE_FRAME_SLANG
#define ENGINE_CORE_FRAME_SLANG

#include "types.slang"
#include "core/binding.slang"

// everything a draw needs, resolved from the push data through the frame root
struct DrawContext
{
    GpuFrameRoot root;
    GpuObjectUB instance;
    GpuModel model;
    GpuPrimitive primitive;
};

DrawContext loadDrawContext(MeshPushData push)
{
    DrawContext draw;
    draw.root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    draw.instance = bdaPtr<GpuObjectUB>(draw.root.instances)[push.instanceIndex];
    draw.model = bdaPtr<GpuModel>(draw.root.models)[draw.instance.modelIndex];
    draw.primitive = bdaPtr<GpuPrimitive>(draw.root.primitives)[push.primitiveIndex];
    return draw;
}

GpuCameraData loadCamera(GpuFrameRoot root)
{
    return bdaPtr<GpuCameraData>(root.camera)[0];
}

GpuMaterial loadMaterial(DrawContext draw)
{
    if (draw.primitive.materialId == kNoneIndex)
    {
        return bdaPtr<GpuMaterial>(draw.root.materials)[0];
    }
    return bdaPtr<GpuMaterial>(draw.model.materials)[draw.primitive.materialId];
}

// parallel to loadMaterial: extension rows share the material row index
GpuMaterialExtension loadMaterialExtension(DrawContext draw)
{
    if (draw.primitive.materialId == kNoneIndex)
    {
        return bdaPtr<GpuMaterialExtension>(draw.root.pbrExtensions)[0];
    }
    return bdaPtr<GpuMaterialExtension>(draw.model.pbrExtensions)[draw.primitive.materialId];
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
