module math;

import types;

public float4x4 objectToClip(GpuCameraData camera, float4x4 worldMatrix)
{
    return mul(camera.proj, mul(camera.view, worldMatrix));
}

// raster jitter as a clip-space offset (engine spec §12.2): raster and the resolve's barycentrics use it, culling
// and motion vectors do not. jitterOffset is in NDC units, zero until DLSS (M3)
public float4 jitterClip(GpuCameraData camera, float4 clip)
{
    clip.xy += camera.jitterOffset * clip.w;
    return clip;
}

// pixel center in NDC; y grows down like the framebuffer (proj[1][1] is flipped)
public float2 pixelToNdc(float2 pixel, float2 targetSize)
{
    return pixel / targetSize * 2.0 - 1.0;
}

// inverse-transpose of the upper 3x3 up to a positive scale: normals stay outward under mirroring
public float3x3 normalMatrix(float4x4 world)
{
    // m[i] is row i; the cofactor rows are the rows of det * inverse-transpose
    const float3 r0 = world[0].xyz;
    const float3 r1 = world[1].xyz;
    const float3 r2 = world[2].xyz;
    const float3x3 cofactor = float3x3(cross(r1, r2), cross(r2, r0), cross(r0, r1));
    const float determinantSign = dot(r0, cross(r1, r2)) < 0.0 ? -1.0 : 1.0;
    return cofactor * determinantSign;
}

// octahedral unit vector <-> [-1, 1]^2
public float2 octEncode(float3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    const float2 folded = (1.0 - abs(n.yx)) * select(n.xy >= 0.0, float2(1.0, 1.0), float2(-1.0, -1.0));
    return n.z >= 0.0 ? n.xy : folded;
}

public float3 octDecode(float2 e)
{
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    const float t = saturate(-n.z);
    n.xy += select(n.xy >= 0.0, float2(-t, -t), float2(t, t));
    return normalize(n);
}
