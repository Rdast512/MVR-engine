#ifndef ENGINE_CORE_MATH_SLANG
#define ENGINE_CORE_MATH_SLANG

#include "types.slang"

float4x4 objectToClip(GpuCameraData camera, GpuObjectUB object)
{
    return mul(camera.proj, mul(camera.view, object.modelMatrix));
}

#endif
