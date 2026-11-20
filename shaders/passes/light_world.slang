import types;
import core.binding;
import core.frame;
import geometry.mesh;
import geometry.surface;
import material.pbr;

// model-space light rows -> this frame's world-space rows (PBR plan E2), one thread per light, async compute before
// the TLAS build. Lights follow their entity without a CPU loop. Row order (WorldLights): the asset's point and spot
// rows, the triangle lights (spec/m7/e2_emissive_triangles.md), the asset's directional rows, the engine's sun (PBR
// plan E3). Triangle rows port PrepareLights.hlsl of the RTXDI sample (reference/RTXDI/Samples/FullSample/Shaders;
// NVIDIA RTX SDKs license): the thread's task by binary search, three corners through the node instance's matrix,
// the emissive texture sampled once at the uv centroid

static const uint kLightWorldGroupSize = 64; // matches src/render/vk_world_lights.cpp

[[vk::push_constant]] ConstantBuffer<LightWorldPushData> push;

// the task holding triangle light `index`: firstLight <= index < firstLight + triangleCount (FindTask)
bool findTask(uint index, out GpuTriangleLightTask task)
{
    task = {};
    GpuTriangleLightTask* tasks = bdaPtr<GpuTriangleLightTask>(push.triangleLightTasks);
    int left = 0;
    int right = int(push.triangleLightTaskCount) - 1;
    while (left <= right)
    {
        const int middle = (left + right) / 2;
        task = tasks[middle];
        if (index < task.firstLight)
        {
            right = middle - 1;
        }
        else if (index - task.firstLight < task.triangleCount)
        {
            return true;
        }
        else
        {
            left = middle + 1;
        }
    }
    return false;
}

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

// emissiveFactor x the emissive texture once per triangle, clamped at 0
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
    centroidFootprint(uv, surface.uv[set], surface.uvDdx[set], surface.uvDdy[set]);
    return max(sampleEmissive(material, surface, 0.0), 0.0);
}

// triangle light `index` of the triangle region, in world space
GpuTriangleLight triangleLight(GpuFrameRoot root, uint index)
{
    GpuTriangleLight light = {};
    light.type = LightType.Triangle;
    GpuTriangleLightTask task;
    if (!findTask(index, task))
    {
        return light;
    }
    const GpuRayInstance instance = bdaPtr<GpuRayInstance>(root.rayInstances)[task.rayInstance];
    const GpuPrimitive primitive = bdaPtr<GpuPrimitive>(root.primitives)[instance.primitiveRow];
    const uint modelIndex = bdaPtr<GpuNodeInstance>(root.nodeInstances)[instance.nodeInstanceIndex].modelIndex;
    GpuMeshletDesc* meshlets = bdaPtr<GpuMeshletDesc>(bdaPtr<GpuModel>(root.models)[modelIndex].meshlets);
    uint triangle;
    GpuVisibleMeshlet visible;
    visible.nodeInstanceIndex = instance.nodeInstanceIndex;
    visible.meshletRow = findMeshlet(meshlets, primitive, index - task.firstLight, triangle);
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

[shader("compute")]
[numthreads(kLightWorldGroupSize, 1, 1)]
void lightWorldMain(uint3 id : SV_DispatchThreadID)
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    if (id.x >= root.lightCount)
    {
        return;
    }
    const uint triangleIndex = id.x - root.firstTriangleLight;
    if (id.x >= root.firstTriangleLight && triangleIndex < root.triangleLightCount)
    {
        bdaPtr<GpuTriangleLight>(root.worldLights)[id.x] = triangleLight(root, triangleIndex);
        return;
    }
    // asset rows keep their order around the triangle region
    const uint assetRow = id.x < root.firstTriangleLight ? id.x : id.x - root.triangleLightCount;
    GpuLight light;
    if (assetRow < push.assetLightCount)
    {
        light = toWorldLight(root, bdaPtr<GpuLight>(root.lights)[assetRow]);
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
