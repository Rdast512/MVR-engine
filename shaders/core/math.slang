module math;

import types;

public static const float kPi = 3.14159265358979323846;

// rec. 709 weights, linear input
public float luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

// duff et al. 2017: continuous except at n.z = 0, no singular pole
public void buildOrthonormalBasis(float3 n, out float3 t, out float3 b)
{
    const float s = n.z >= 0.0 ? 1.0 : -1.0;
    const float a = -1.0 / (s + n.z);
    const float c = n.x * n.y * a;
    t = float3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = float3(c, s + n.y * n.y * a, -n.y);
}

// the one object-to-clip product: raster and the resolve must see bit-identical corners
public float4x4 objectToClip(GpuCameraData camera, float4x4 worldMatrix)
{
    return mul(camera.viewProj, worldMatrix);
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

// octahedral unit vector <-> packed snorm16x2 (one R32_UINT word, < 0.0025 deg error)
public uint packOct16(float3 n)
{
    const int2 s = int2(round(clamp(octEncode(n), -1.0, 1.0) * 32767.0));
    return (uint(s.x) & 0xffffu) | ((uint(s.y) & 0xffffu) << 16);
}

public static const uint kPackedOctZero = 0x80008000u;

public float3 unpackOct16(uint packed)
{
    const int sx = int(packed << 16) >> 16;
    const int sy = int(packed) >> 16;
    return octDecode(float2(sx, sy) * (1.0 / 32767.0));
}

public float3 unpackOct16OrZero(uint packed)
{
    return packed == kPackedOctZero ? float3(0.0, 0.0, 0.0) : unpackOct16(packed);
}

// octahedral snorm15x2 in bits [0..14] / [15..29], bit 30 = negative handedness, bit 31 = valid (0 = absent)
public float4 unpackTangent32(uint packed)
{
    if ((packed & 0x80000000u) == 0u)
    {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    const int sx = int(packed << 17) >> 17;
    const int sy = int(packed << 2) >> 17;
    const float sign = (packed & 0x40000000u) != 0u ? -1.0 : 1.0;
    return float4(octDecode(float2(sx, sy) * (1.0 / 16383.0)), sign);
}

public float2 unpackHalf2x16(uint packed)
{
    return float2(f16tof32(packed & 0xffffu), f16tof32(packed >> 16));
}

public float3 unpackColorUnorm8(uint packed)
{
    return float3(packed & 0xffu, (packed >> 8) & 0xffu, (packed >> 16) & 0xffu) * (1.0 / 255.0);
}

// f0.rgb as unorm8x3; top 8 bits: 0 = unlit, 1..255 = lit with roughness (a - 1) / 254 (m8 O4)
public uint packSurfaceMaterial(float3 f0, float roughness, bool isLit)
{
    const uint3 qF0 = uint3(round(saturate(f0) * 255.0));
    const uint qAlpha = isLit ? (1u + uint(round(saturate(roughness) * 254.0))) : 0u;
    return qF0.r | (qF0.g << 8) | (qF0.b << 16) | (qAlpha << 24);
}

public void unpackSurfaceMaterial(uint packed, out float3 f0, out float roughness, out bool isLit)
{
    f0 = float3(packed & 0xffu, (packed >> 8) & 0xffu, (packed >> 16) & 0xffu) * (1.0 / 255.0);
    const uint qAlpha = packed >> 24;
    isLit = qAlpha != 0u;
    roughness = isLit ? float(qAlpha - 1u) * (1.0 / 254.0) : 0.0;
}
