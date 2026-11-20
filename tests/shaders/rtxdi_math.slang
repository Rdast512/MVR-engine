// tests RTXDI bridge GI math, LogLuv reservoir packing, Jacobian validation and CheckedBuffer bounds guards
import types;
import core.binding;
import core.math;
import material.brdf;

#include "lighting/rtxdi_bridge.slang"
#include "Rtxdi/GI/Reservoir.hlsli"

// scalar layout: must match RtxdiMathPush in tests/rtxdi_math_test.cpp
struct RtxdiMathPush
{
    float4* radianceInputs;   // radianceCount rows: rgb = input radiance, w = weightSum
    float4* radianceOutputs;  // radianceCount rows: rgb = unpacked radiance, w = unpacked weightSum
    float* jacobianInputs;    // jacobianCount values
    float4* jacobianOutputs;  // jacobianCount rows: x = valid (0/1), y = clamped jacobian
    float4* shadeOutputs;     // 4 rows: [0]=lit rgb + targetPdf, [1]=backfacing, [2]=unlit, [3]=bounds guard results
    uint* guardedBuffer;      // 4 uints for CheckedBuffer test
    uint radianceCount;
    uint jacobianCount;
};

[[vk::push_constant]] ConstantBuffer<RtxdiMathPush> push;

[shader("compute")]
[numthreads(64, 1, 1)]
void rtxdiMathMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x < push.radianceCount)
    {
        const float4 inp = push.radianceInputs[id.x];
        RTXDI_GIReservoir r = RTXDI_EmptyGIReservoir();
        r.position = float3(1.5, -2.25, 3.75);
        r.normal = normalize(float3(0.3, 0.7, -0.6));
        r.radiance = inp.rgb;
        r.weightSum = inp.w;
        r.M = 12;
        r.age = 7;
        const RTXDI_PackedGIReservoir packed = RTXDI_PackGIReservoir(r, 0);
        const RTXDI_GIReservoir unpacked = RTXDI_UnpackGIReservoir(packed);
        push.radianceOutputs[id.x] = float4(unpacked.radiance, unpacked.weightSum);
    }

    if (id.x < push.jacobianCount)
    {
        float j = push.jacobianInputs[id.x];
        const bool ok = RAB_ValidateGISampleWithJacobian(j);
        push.jacobianOutputs[id.x] = float4(ok ? 1.0 : 0.0, j, 0.0, 0.0);
    }

    if (id.x == 0)
    {
        RAB_Surface surface;
        surface.position = float3(0.0, 0.0, 0.0);
        surface.viewDepth = 2.0;
        surface.normal = float3(0.0, 0.0, 1.0);
        surface.geoNormal = float3(0.0, 0.0, 1.0);
        surface.viewDir = normalize(float3(0.5, 0.0, 0.8660254)); // 30 deg from +Z
        surface.diffuseAlbedo = float3(0.7, 0.25, 0.1);
        surface.specularF0 = float3(0.04, 0.04, 0.04);
        surface.roughness = 0.4;
        surface.isLit = true;

        const float3 samplePos = float3(-1.0, 0.5, 1.5);
        const float3 sampleRad = float3(2.0, 1.5, 0.5);
        const float3 shaded = shadeGiSample(surface, samplePos, sampleRad);
        const float targetPdf = RAB_GetGISampleTargetPdfForSurface(samplePos, sampleRad, surface);
        push.shadeOutputs[0] = float4(shaded, targetPdf);

        // backfacing sample (below the shading hemisphere) must return zero
        const float3 behindShaded = shadeGiSample(surface, float3(0.0, 0.0, -2.0), sampleRad);
        push.shadeOutputs[1] = float4(behindShaded, RAB_GetGISampleTargetPdfForSurface(float3(0.0, 0.0, -2.0), sampleRad, surface));

        // unlit primary surface must return zero
        RAB_Surface unlit = surface;
        unlit.isLit = false;
        const float3 unlitShaded = shadeGiSample(unlit, samplePos, sampleRad);
        push.shadeOutputs[2] = float4(unlitShaded, RAB_GetGISampleTargetPdfForSurface(samplePos, sampleRad, unlit));

        // CheckedBuffer: in-bounds read/write succeeds; out-of-bounds read returns 0 and write is dropped (C-F3-1)
        CheckedBuffer<uint> checked;
        checked.address = uint64_t(push.guardedBuffer);
        checked.count = 4;
        checked[2] = 0x12345678u;
        checked[4] = 0xdeadbeefu;
        checked[9999u] = 0xdeadbeefu;
        const uint inBounds = checked[2];
        const uint oobAtEnd = checked[4];
        const uint oobFar = checked[9999u];
        const uint intact = checked[3];
        push.shadeOutputs[3] = float4(float(inBounds == 0x12345678u), float(oobAtEnd == 0u), float(oobFar == 0u),
                                      float(intact == 42u));
    }
}
