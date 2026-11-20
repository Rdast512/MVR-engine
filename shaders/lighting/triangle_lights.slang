module triangle_lights;

import types;
import core.binding;
import geometry.hit;

// emissive-triangle lights (spec/m7/e2_emissive_triangles.md): world rows of type Triangle, a ray hit -> the light it
// names, and the area sample RTXDI scores. The sampling follows TriangleLight in the RTXDI sample
// (reference/RTXDI/Samples/FullSample/Shaders/PolymorphicLight.hlsli, HelperFunctions.hlsli; NVIDIA RTX SDKs license),
// with one deviation: a double-sided material emits from both faces

// what a hit needs to name its light; filled once per dispatch
public struct TriangleLightTable
{
    public uint64_t rayInstances;      // GpuRayInstance[]
    public uint64_t rayInstanceLights; // uint[], parallel: first triangle-region row or kNoneIndex
    public uint64_t nodeInstances;
    public uint64_t models;
    public uint64_t primitives;
    public uint firstLight; // world row of triangle light 0
    public uint lightCount; // 0 while the triangle lights are not sampled: no hit names a light
};

public TriangleLightTable loadTriangleLightTable(GpuFrameRoot root)
{
    TriangleLightTable table;
    table.rayInstances = root.rayInstances;
    table.rayInstanceLights = root.rayInstanceLights;
    table.nodeInstances = root.nodeInstances;
    table.models = root.models;
    table.primitives = root.primitives;
    table.firstLight = root.firstTriangleLight;
    table.lightCount = (root.frameFlags & kFrameFlagTriangleLightsUnsampled) != 0 ? 0 : root.triangleLightCount;
    return table;
}

// a ray instance's first triangle-region row; kNoneIndex unless its material emits
public uint rayInstanceFirstLight(TriangleLightTable table, uint rayInstance)
{
    if (table.lightCount == 0)
    {
        return kNoneIndex;
    }
    return bdaPtr<uint>(table.rayInstanceLights)[rayInstance];
}

// true when every triangle of the hit's primitive is a sampled light (one emission rule)
public bool isLightHit(TriangleLightTable table, RayHit hit)
{
    return rayInstanceFirstLight(table, hit.rayInstance) != kNoneIndex;
}

// world row of the triangle light a hit names, or kNoneIndex. A primitive's triangles are stored in meshlet order
// (GeometryStore), so the hit meshlet's triangleOffset gives its place in the primitive
public uint hitLightIndex(TriangleLightTable table, RayHit hit)
{
    const uint firstLight = rayInstanceFirstLight(table, hit.rayInstance);
    if (firstLight == kNoneIndex)
    {
        return kNoneIndex;
    }
    const GpuRayInstance instance = bdaPtr<GpuRayInstance>(table.rayInstances)[hit.rayInstance];
    const uint modelIndex = bdaPtr<GpuNodeInstance>(table.nodeInstances)[instance.nodeInstanceIndex].modelIndex;
    GpuMeshletDesc* meshlets = bdaPtr<GpuMeshletDesc>(bdaPtr<GpuModel>(table.models)[modelIndex].meshlets);
    const uint firstMeshlet = bdaPtr<GpuPrimitive>(table.primitives)[instance.primitiveRow].firstMeshlet;
    const uint corner = meshlets[hit.meshletRow].triangleOffset - meshlets[firstMeshlet].triangleOffset;
    return table.firstLight + firstLight + corner / 3 + hit.triangle;
}

// uniform point on a triangle: barycentrics of (v0, v1, v2) from u in [0, 1)^2 (SampleTriangle)
public float3 sampleTriangleBarycentrics(float2 u)
{
    const float root = sqrt(u.x);
    return float3(1.0 - root, root * (1.0 - u.y), root * u.y);
}

// inverse of sampleTriangleBarycentrics (RandomFromBarycentric), so a BRDF hit is a sample RTXDI can replay
public float2 triangleRandFromBarycentrics(float3 barycentrics)
{
    const float root = 1.0 - barycentrics.x;
    return float2(root * root, root > 0.0 ? barycentrics.z / root : 0.0);
}

// a world row read as the triangle it holds (type Triangle)
public GpuTriangleLight asTriangleLight(GpuLight light)
{
    return reinterpret<GpuTriangleLight>(light);
}

public struct TriangleLightSample
{
    public float3 position;
    public float3 l; // unit, toward the light
    public float distance;
    // solid-angle pdf of the point seen from the surface; 0: the surface sees no emitting face
    public float solidAnglePdf;
    public float3 radiance; // emitted toward the surface
};

// the point u maps to, as seen from surfacePosition
public TriangleLightSample sampleTriangleLight(GpuTriangleLight light, float3 surfacePosition, float2 u)
{
    const float3 barycentrics = sampleTriangleBarycentrics(u);
    TriangleLightSample sample = {};
    sample.position = light.v0 + light.edge1 * barycentrics.y + light.edge2 * barycentrics.z;
    const float3 toLight = sample.position - surfacePosition;
    sample.distance = length(toLight);
    const float3 scaledNormal = cross(light.edge1, light.edge2);
    const float doubleArea = length(scaledNormal);
    if (sample.distance <= 0.0 || doubleArea <= 0.0)
    {
        return sample;
    }
    sample.l = toLight / sample.distance;
    // cross(edge1, edge2) is the front face's normal unless the placement mirrors
    const float facing = (light.flags & kTriangleLightFlagMirrored) != 0 ? -1.0 : 1.0;
    const float signedCosine = facing * dot(sample.l, -scaledNormal) / doubleArea;
    const float cosine = (light.flags & kTriangleLightFlagDoubleSided) != 0 ? abs(signedCosine) : signedCosine;
    if (cosine <= 0.0)
    {
        return sample;
    }
    // area pdf 1 / area to solid angle: d^2 / (cos * area) (PdfAtoW)
    sample.solidAnglePdf = sample.distance * sample.distance / (cosine * 0.5 * doubleArea);
    sample.radiance = light.radiance;
    return sample;
}
