// runs lighting/lights.slang and core/frame.slang::toWorldLight on known lights; tests/lights_test.cpp checks them
import types;
import core.frame;
import lighting.lights;

// scalar layout: must match LightCase in tests/lights_test.cpp
struct LightCase
{
    GpuLight light;
    float3 p;
    uint useWorldTransform;
};

// scalar layout: must match LightPush in tests/lights_test.cpp
struct LightPush
{
    LightCase* cases;
    float4* results; // kResultRows per case
    GpuFrameRoot* frameRoot;
    uint caseCount;
    uint pad0;
};

static const uint kResultRows = 4;

[[vk::push_constant]] ConstantBuffer<LightPush> push;

[shader("compute")]
[numthreads(64, 1, 1)]
void lightsMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= push.caseCount)
    {
        return;
    }
    const LightCase c = push.cases[id.x];
    const GpuLight light = c.useWorldTransform != 0 ? toWorldLight(push.frameRoot[0], c.light) : c.light;
    const LightSample s = evalLight(light, c.p);
    const uint row = id.x * kResultRows;
    push.results[row + 0] = float4(s.radiance, s.distance);
    push.results[row + 1] = float4(s.l, light.innerCone);
    push.results[row + 2] = float4(light.position, light.range);
    push.results[row + 3] = float4(light.direction, light.intensity);
}
