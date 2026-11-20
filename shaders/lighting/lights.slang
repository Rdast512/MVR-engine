module lights;

import types;
import core.math;

// KHR_lights_punctual evaluation; the resolve, rtxdi bridge and indirect pass all call this, none re-derives it

// flt max: a valid ray tMax, unlike inf in arithmetic
public static const float kInfiniteDistance = 3.402823466e38;

// khronos floor on d² so a light at the shading point stays finite
static const float kMinDistanceSquared = 1e-4;
static const float kMinConeCosDelta = 1e-3;

public struct LightSample
{
    public float3 l;        // unit, toward the light
    public float3 radiance; // incident radiance before NdotL
    public float distance;  // shadow ray tMax
};

// khronos window: 1 - (d/range)^4 clamped; range 0 means infinite
public float rangeWindow(float d, float range)
{
    if (range <= 0.0)
    {
        return 1.0;
    }
    const float r2 = (d * d) / (range * range);
    return saturate(1.0 - r2 * r2);
}

// cosToAxis = dot(spotDir, -l): +1 on axis
public float spotCone(float cosToAxis, float innerCone, float outerCone)
{
    const float cosOuter = cos(outerCone);
    const float scale = 1.0 / max(kMinConeCosDelta, cos(innerCone) - cosOuter);
    const float t = saturate((cosToAxis - cosOuter) * scale);
    return t * t;
}

// light in world space (loadWorldLight): unit direction, no node scale on intensity, range or cone
public LightSample evalLight(GpuLight light, float3 p)
{
    LightSample s;
    if (light.type == LightType.Directional)
    {
        s.l = -light.direction;
        s.radiance = light.color * light.intensity; // lux
        s.distance = kInfiniteDistance;
        return s;
    }

    const float3 toLight = light.position - p;
    const float d2 = max(dot(toLight, toLight), 1e-12);
    s.distance = sqrt(d2);
    s.l = toLight / s.distance;

    float attenuation = rangeWindow(s.distance, light.range) / max(d2, kMinDistanceSquared);
    if (light.type == LightType.Spot)
    {
        attenuation *= spotCone(dot(light.direction, -s.l), light.innerCone, light.outerCone);
    }
    s.radiance = light.color * (light.intensity * attenuation); // candela
    return s;
}

// RTXDI sample (PolymorphicLight.hlsli GetAverageDistanceToVolume; NVIDIA RTX SDKs license): the mean distance from a
// point to a random point of a sphere, fitted to a Monte Carlo curve; 0.75 r at the center, d far away
public float averageDistanceToVolume(float distanceToCenter, float volumeRadius)
{
    const float kNonlinearFactor = 1.1547;
    const float spread = distanceToCenter + volumeRadius * kNonlinearFactor;
    return distanceToCenter + volumeRadius * volumeRadius * volumeRadius / (spread * spread);
}

// whether a sphere meets a cone from its apex (TestSphereConeIntersection in the RTXDI sample's LightShaping.hlsli)
bool sphereMeetsCone(float3 apex, float3 axis, float halfAngle, float3 center, float radius)
{
    const float3 toCenter = center - apex;
    const float distance = length(toCenter);
    if (distance <= radius)
    {
        return true;
    }
    const float angleToAxis = acos(clamp(dot(toCenter, axis) / distance, -1.0, 1.0));
    return angleToAxis <= asin(radius / distance) + halfAngle;
}

// a punctual light's weight for every surface inside a sphere (PointLight::GetWeightForVolume): I / d^2 at the mean
// distance. Zero only where no point of the sphere can receive light: past the range, outside a spot's outer cone
public float punctualLightWeightForVolume(GpuLight light, float3 center, float radius)
{
    const float distance = length(center - light.position);
    if (light.range > 0.0 && distance - radius >= light.range)
    {
        return 0.0;
    }
    if (light.type == LightType.Spot && !sphereMeetsCone(light.position, light.direction, light.outerCone, center,
                                                         radius))
    {
        return 0.0;
    }
    const float meanDistance = averageDistanceToVolume(distance, radius);
    return luminance(light.color * light.intensity) / (meanDistance * meanDistance);
}
