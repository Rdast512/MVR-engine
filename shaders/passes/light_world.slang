import types;
import core.binding;
import core.frame;
import core.math;
import geometry.mesh;
import geometry.surface;
import lighting.compact_lights;
import material.pbr;

// RTXDI_LinearIndexToZCurve: the PDF texel of a light must be the one RTXDI's presampling maps back to its index
#include "Rtxdi/Utils/Math.hlsli"

// model-space light rows -> this frame's world-space rows (PBR plan E2), one thread per light, async compute before
// the TLAS build. The same threads write the local light PDF's mip 0 (spec/m7/f1_light_presampling.md): each local
// light's power at its Z-curve texel, every other texel 0, so no texel keeps an earlier frame's value. Lights follow
// their entity without a CPU loop. Row order (WorldLights): the asset's point and spot rows, the triangle lights
// (spec/m7/e2_emissive_triangles.md), the asset's directional rows, the engine's sun (PBR plan E3). Triangle rows port PrepareLights.hlsl of the RTXDI sample (reference/RTXDI/Samples/FullSample/Shaders;
// NVIDIA RTX SDKs license): the thread's task by binary search, three corners through the node instance's matrix,
// the emissive texture sampled once at the uv centroid

static const uint kLightWorldGroupSize = 64; // matches src/render/vk_world_lights.cpp

[[vk::push_constant]] ConstantBuffer<LightWorldPushData> push;

// the meshlet holding the primitive's triangle `index`: its triangles are dense in meshlet order (GeometryStore), so
// the last meshlet whose triangleOffset is at most the triangle's
uint findMeshlet(GpuMeshletDesc* meshlets, GpuPrimitive primitive, uint index, out uint triangle)
{
    const uint corner = meshlets[primitive.firstMeshlet].triangleOffset + index * 3;
    uint low = primitive.firstMeshlet;
    uint high = primitive.firstMeshlet + primitive.meshletCount - 1;
    while (low < high)
    {
        const uint middle = (low + high + 1) / 2;
        if (meshlets[middle].triangleOffset <= corner)
        {
            low = middle;
        }
        else
        {
            high = middle - 1;
        }
    }
    triangle = (corner - meshlets[low].triangleOffset) / 3;
    return low;
}

// the sample's anisotropic footprint at the uv centroid: axes along the shortest uv edge and the median from its
// opposite corner, each 2/3 of it, about the inscribed ellipse. Deviation: the sample's second axis sums the two long
// edges, which is the short edge reversed; the median is their difference
void centroidFootprint(float2 uv[3], out float2 center, out float2 ddx, out float2 ddy)
{
    const float2 edges[3] = { uv[1] - uv[0], uv[2] - uv[1], uv[0] - uv[2] };
    const float3 lengths = float3(length(edges[0]), length(edges[1]), length(edges[2]));
    uint shortest = 2;
    if (lengths[0] < lengths[1] && lengths[0] < lengths[2])
    {
        shortest = 0;
    }
    else if (lengths[1] < lengths[2])
    {
        shortest = 1;
    }
    const float2 longEdge1 = edges[(shortest + 1) % 3];
    const float2 longEdge2 = edges[(shortest + 2) % 3];
    center = (uv[0] + uv[1] + uv[2]) / 3.0;
    ddx = edges[shortest] * (2.0 / 3.0);
    ddy = (longEdge2 - longEdge1) / 3.0;
}

// emissiveFactor x the emissive texture averaged over 4 equal-area sub-triangles at half footprint (m8 I14 F27):
// reduces coarse-mip bleed across UV-atlas boundaries on large housing triangles and catches partial emitters
float3 triangleRadiance(GpuMaterial material, DrawContext draw, TriangleCorners corners)
{
    if (material.emissiveTex == kNoneIndex || material.emissiveSamp == kNoneIndex)
    {
        return max(material.emissiveFactor, 0.0);
    }
    Surface surface = {};
    float2 uv[3] = corners.texCoord;
    if (material.emissiveUv != 0)
    {
        [unroll]
        for (uint i = 0; i < 3; ++i)
        {
            uv[i] = loadVertexUv1(draw, corners.vertexIndex[i]);
        }
    }
    const uint set = min(material.emissiveUv, 1u);
    float2 center;
    float2 ddx;
    float2 ddy;
    centroidFootprint(uv, center, ddx, ddy);
    // each of the 4 equal-area sub-triangles has half the linear span; cap footprint to prevent full-atlas mip bleed
    const float2 subDdx = ddx * 0.5;
    const float2 subDdy = ddy * 0.5;
    surface.uvDdx[set] = subDdx;
    surface.uvDdy[set] = subDdy;
    const float2 taps[4] = {
        center,
        (4.0 * uv[0] + uv[1] + uv[2]) / 6.0,
        (uv[0] + 4.0 * uv[1] + uv[2]) / 6.0,
        (uv[0] + uv[1] + 4.0 * uv[2]) / 6.0,
    };
    float3 sum = float3(0.0, 0.0, 0.0);
    [unroll]
    for (uint i = 0; i < 4; ++i)
    {
        surface.uv[set] = taps[i];
        sum += max(sampleEmissive(material, surface, 0.0), 0.0);
    }
    return sum * 0.25;
}

// triangle light `index` of the triangle region, in world space
GpuTriangleLight triangleLight(GpuFrameRoot root, uint index)
{
    GpuTriangleLight light = {};
    light.type = LightType.Triangle;
    if (index >= push.triangleLightRefCount)
    {
        return light;
    }
    const GpuTriangleLightRef ref = bdaPtr<GpuTriangleLightRef>(push.triangleLightRefs)[index];
    const GpuRayInstance instance = bdaPtr<GpuRayInstance>(root.rayInstances)[ref.rayInstance];
    const GpuPrimitive primitive = bdaPtr<GpuPrimitive>(root.primitives)[instance.primitiveRow];
    const uint modelIndex = bdaPtr<GpuNodeInstance>(root.nodeInstances)[instance.nodeInstanceIndex].modelIndex;
    GpuMeshletDesc* meshlets = bdaPtr<GpuMeshletDesc>(bdaPtr<GpuModel>(root.models)[modelIndex].meshlets);
    uint triangle;
    GpuVisibleMeshlet visible;
    visible.nodeInstanceIndex = instance.nodeInstanceIndex;
    visible.meshletRow = findMeshlet(meshlets, primitive, ref.localTriangle, triangle);
    const DrawContext draw = loadDrawContext(root, visible, kNoneIndex);

    // meshlet corner order, as the CLAS the hits come from: hit barycentrics map onto (v0, edge1, edge2)
    const TriangleCorners corners = loadCornerVertices(draw, triangle);
    const float4x4 world = draw.nodeInstance.worldMatrix;
    float3 positions[3];
    [unroll]
    for (uint i = 0; i < 3; ++i)
    {
        positions[i] = mul(world, float4(corners.objectPos[i], 1.0)).xyz;
    }
    light.v0 = positions[0];
    light.edge1 = positions[1] - positions[0];
    light.edge2 = positions[2] - positions[0];
    if (draw.isDoubleSided)
    {
        light.flags |= kTriangleLightFlagDoubleSided;
    }
    if (determinant((float3x3)world) < 0.0)
    {
        light.flags |= kTriangleLightFlagMirrored;
    }
    light.radiance = triangleRadiance(loadMaterial(root, materialRow(primitive)), draw, corners);
    return light;
}

// world row `index`: a triangle light, or an asset row or the sun around the triangle region
GpuLight worldLight(GpuFrameRoot root, uint index)
{
    const uint triangleIndex = index - root.firstTriangleLight;
    if (index >= root.firstTriangleLight && triangleIndex < root.triangleLightCount)
    {
        return reinterpret<GpuLight>(triangleLight(root, triangleIndex));
    }
    const uint assetRow = index < root.firstTriangleLight ? index : index - root.triangleLightCount;
    if (assetRow < push.assetLightCount)
    {
        return toWorldLight(root, bdaPtr<GpuLight>(root.lights)[assetRow]);
    }
    GpuLight sun = {};
    sun.direction = push.sunDirection;
    sun.intensity = push.sunIlluminance;
    sun.color = push.sunColor;
    sun.type = LightType.Directional;
    return sun;
}

// emitted flux, the PDF's weight (PolymorphicLight GetPower in the RTXDI sample). Triangles: area · π · L, both faces
// of a double-sided one. Point and spot: 4π · I; the spot's cone shapes where the flux goes, not how much is sampled
float lightPower(GpuLight light)
{
    if (light.type == LightType.Triangle)
    {
        const GpuTriangleLight triangle = reinterpret<GpuTriangleLight>(light);
        const float area = 0.5 * length(cross(triangle.edge1, triangle.edge2));
        const float faces = (triangle.flags & kTriangleLightFlagDoubleSided) != 0 ? 2.0 : 1.0;
        return area * kPi * faces * luminance(triangle.radiance);
    }
    return 4.0 * kPi * luminance(light.color * light.intensity);
}

// one thread per world row and per PDF texel, whichever is more
[shader("compute")]
[numthreads(kLightWorldGroupSize, 1, 1)]
void lightWorldMain(uint3 id : SV_DispatchThreadID)
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    float power = 0.0;
    if (id.x < root.lightCount)
    {
        GpuLight light = worldLight(root, id.x);
        // M8.1 BUG-A4: canonicalize compactable lights so worldLights and risLightData share one quantized geometry
        uint4 compact0;
        uint4 compact1;
        if (packCompactLight(light, compact0, compact1))
        {
            light = unpackCompactLight(compact0, compact1);
        }
        bdaPtr<GpuLight>(root.worldLights)[id.x] = light;
        // the local region: RTXDI never selects a texel past it
        if (id.x < root.localLightCount)
        {
            power = lightPower(light);
        }
    }
    if (id.x < push.lightPdfTexelCount)
    {
        // RtxdiContext's PDF format
        storageTex2D<float, kImageFormatR32f>(push.lightPdf)[RTXDI_LinearIndexToZCurve(id.x)] = power;
    }
}
