module compact_lights;

import types;
import core.math;

// a world light row in 32 B (two uint4), carried by each RIS entry so RTXDI's initial sampling skips the world-row load
// (RAB_StoreCompactLightInfo / RAB_LoadCompactLightInfo; spec/m7/f1_light_presampling.md). Triangles are packed as the
// RTXDI sample packs them (PolymorphicLight.hlsli TriangleLight::Store, PackCompactLightInfo; NVIDIA RTX SDKs license):
// the centroid, two octahedral edge directions and two f16 lengths. A point light keeps its own fields. Spot lights are
// not packed, as the sample's shaped lights: their cone needs more than 32 B. Radiance and color are f16
//
//   triangle  first  = centroid.xyz, oct(edge1)          second = oct(edge2), f16 lengths, f16 rg, f16 b | header
//   point     first  = position.xyz, intensity          second = range, 0, f16 rg, f16 b | header
//
// header (high 16 bits of second.w): LightType in bits 0-7, GpuTriangleLight.flags in bits 8-15

static const float kHalfMax = 65504.0;

// octahedral direction in two unorm16 (ndirToOctUnorm32)
uint packOctUnorm32(float3 direction)
{
    const float2 e = saturate(octEncode(direction) * 0.5 + 0.5);
    const uint2 q = uint2(round(e * 65535.0));
    return q.x | (q.y << 16);
}

float3 unpackOctUnorm32(uint packed)
{
    const float2 e = float2(float(packed & 0xffffu), float(packed >> 16)) * (2.0 / 65535.0) - 1.0;
    return octDecode(e);
}

uint packHalf2(float a, float b)
{
    return f32tof16(a) | (f32tof16(b) << 16);
}

// a zero-length edge keeps any direction
uint packEdgeDirection(float3 edge, float length)
{
    return packOctUnorm32(length > 0.0 ? edge / length : float3(0.0, 0.0, 1.0));
}

// false: the row stays uncompacted and RTXDI loads it from the world rows
public bool packCompactLight(GpuLight light, out uint4 first, out uint4 second)
{
    first = uint4(0, 0, 0, 0);
    second = uint4(0, 0, 0, 0);
    float3 color;
    uint header = uint(light.type);
    if (light.type == LightType.Triangle)
    {
        const GpuTriangleLight triangle = reinterpret<GpuTriangleLight>(light);
        const float length1 = length(triangle.edge1);
        const float length2 = length(triangle.edge2);
        if (max(length1, length2) > kHalfMax)
        {
            return false;
        }
        const float3 centroid = triangle.v0 + (triangle.edge1 + triangle.edge2) / 3.0;
        first = uint4(asuint(centroid), packEdgeDirection(triangle.edge1, length1));
        second.x = packEdgeDirection(triangle.edge2, length2);
        second.y = packHalf2(length1, length2);
        color = triangle.radiance;
        header |= (triangle.flags & 0xffu) << 8;
    }
    else if (light.type == LightType.Point)
    {
        first = uint4(asuint(light.position), asuint(light.intensity));
        second.x = asuint(light.range);
        color = light.color;
    }
    else
    {
        return false;
    }
    if (any(color > kHalfMax))
    {
        return false;
    }
    second.z = packHalf2(color.r, color.g);
    second.w = f32tof16(color.b) | (header << 16);
    return true;
}

public GpuLight unpackCompactLight(uint4 first, uint4 second)
{
    const uint header = second.w >> 16;
    const float3 color = float3(f16tof32(second.z), f16tof32(second.z >> 16), f16tof32(second.w));
    if ((header & 0xffu) == uint(LightType.Triangle))
    {
        GpuTriangleLight triangle = {};
        triangle.type = LightType.Triangle;
        triangle.flags = header >> 8;
        triangle.edge1 = unpackOctUnorm32(first.w) * f16tof32(second.y);
        triangle.edge2 = unpackOctUnorm32(second.x) * f16tof32(second.y >> 16);
        triangle.v0 = asfloat(first.xyz) - (triangle.edge1 + triangle.edge2) / 3.0;
        triangle.radiance = color;
        return reinterpret<GpuLight>(triangle);
    }
    GpuLight light = {};
    light.type = LightType.Point;
    light.position = asfloat(first.xyz);
    light.intensity = asfloat(first.w);
    light.range = asfloat(second.x);
    light.color = color;
    return light;
}
