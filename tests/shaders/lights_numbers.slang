// runs lighting/lights.slang on known lights; tests/lights_test.cpp checks the results
import types;
import lighting.lights;

// scalar layout: must match LightCase in tests/lights_test.cpp
struct LightCase
{
    GpuLight light;
    float3 p;
    float pad0;
};

// scalar layout: must match LightPush in tests/lights_test.cpp
struct LightPush
{
    LightCase* cases;
    float4* results; // kResultRows per case
    uint caseCount;
};

static const uint kResultRows = 2;

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
    const LightSample s = evalLight(c.light, c.p);
    push.results[id.x * kResultRows + 0] = float4(s.radiance, s.distance);
    push.results[id.x * kResultRows + 1] = float4(s.l, 0.0);
}
