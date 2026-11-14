import types;
import core.binding;
import core.frame;

// model-space light rows -> this frame's world-space rows (PBR plan E2), one thread per light, async compute before
// the TLAS build. Lights follow their entity without a CPU loop. The engine's sun (PBR plan E3) is the last row

static const uint kLightWorldGroupSize = 64; // matches src/render/vk_world_lights.cpp

[[vk::push_constant]] ConstantBuffer<LightWorldPushData> push;

[shader("compute")]
[numthreads(kLightWorldGroupSize, 1, 1)]
void lightWorldMain(uint3 id : SV_DispatchThreadID)
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    if (id.x >= root.lightCount)
    {
        return;
    }
    GpuLight light;
    if (id.x < push.assetLightCount)
    {
        light = toWorldLight(root, bdaPtr<GpuLight>(root.lights)[id.x]);
    }
    else
    {
        light = {};
        light.direction = push.sunDirection;
        light.intensity = push.sunIlluminance;
        light.color = push.sunColor;
        light.type = LightType.Directional;
    }
    bdaPtr<GpuLight>(root.worldLights)[id.x] = light;
}
