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

#endif
