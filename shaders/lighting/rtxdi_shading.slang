// Shared direct-lighting shading helpers for passes/rtxdi_shade.slang and passes/rtxdi_streaming_ris.slang.
// Included after lighting/rtxdi_bridge.slang (and importing lighting.glass).

// m8 I13 §3: light through glass; the reservoir's visibility and the resampling stay glass-free
float3 glassTransmittance(GpuFrameRoot root, RAB_Surface surface, RAB_LightSample sample)
{
    const float3 origin = rasterRayOrigin(surface);
    float3 dir;
    float tMax;
    return lightVisibilitySegment(origin, sample, dir, tMax) ? shadowTransmittance(root, origin, dir, tMax)
                                                             : float3(1.0, 1.0, 1.0);
}

// directional lights are deltas: one shadow ray each shades them exactly, without RIS noise (m8 I16 F28)
float3 shadeInfiniteLightsExactly(GpuFrameRoot root, RAB_Surface surface, RTXDI_LightBufferRegion region)
{
    float3 radiance = float3(0.0, 0.0, 0.0);
    for (uint i = 0; i < region.numLights; ++i)
    {
        const RAB_LightInfo light = RAB_LoadLightInfo(region.firstLightIndex + i, false);
        const RAB_LightSample sample = RAB_SamplePolymorphicLight(light, surface, float2(0.0, 0.0));
        const float3 shade = shadeLightSample(surface, sample);
        if (any(shade > 0.0) && RAB_GetConservativeVisibility(surface, sample))
        {
            radiance += shade * glassTransmittance(root, surface, sample);
        }
    }
    return radiance;
}
