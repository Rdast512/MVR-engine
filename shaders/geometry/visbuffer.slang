module visbuffer;

import types;

// R32_UINT visibility buffer (spec/features/visibility_buffer.md §3.1): (visibleRow << kVisTriangleBits) | triangle.
// Empty pixels are decided by depth (reverse-Z clear 0), never by the payload

static const uint kVisTriangleMask = (1u << kVisTriangleBits) - 1u;

public uint packVisPayload(uint visibleRow, uint triangle)
{
    return (visibleRow << kVisTriangleBits) | triangle;
}

public uint visPayloadRow(uint payload)
{
    return payload >> kVisTriangleBits;
}

public uint visPayloadTriangle(uint payload)
{
    return payload & kVisTriangleMask;
}

// perspective-correct barycentrics at a pixel and their exact screen derivatives on the triangle's plane
public struct Barycentrics
{
    public float3 lambda;
    public float3 ddx; // per pixel step in x
    public float3 ddy; // per pixel step in y
};

// homogeneous edge functions (Olano-Greer): the rows of inverse([a.xyw b.xyw c.xyw]) up to scale. No divide by w,
// so they hold for triangles crossing the eye plane; evaluated at NDC p they give unnormalized barycentrics
float3 edgeWeights(float3 e0, float3 e1, float3 e2, float2 p)
{
    const float3 q = float3(p, 1.0);
    const float3 weights = float3(dot(e0, q), dot(e1, q), dot(e2, q));
    return weights / (weights.x + weights.y + weights.z);
}

// clip corners as the rasterizer saw them (jittered); ndc of the pixel center; one pixel in NDC per axis
public Barycentrics computeBarycentrics(float4 a, float4 b, float4 c, float2 ndc, float2 pixelNdc)
{
    const float3 e0 = cross(b.xyw, c.xyw);
    const float3 e1 = cross(c.xyw, a.xyw);
    const float3 e2 = cross(a.xyw, b.xyw);
    Barycentrics result;
    result.lambda = edgeWeights(e0, e1, e2, ndc);
    // the neighbours' barycentrics on the same plane: no helper lanes, no quad overdraw at edges
    result.ddx = edgeWeights(e0, e1, e2, ndc + float2(pixelNdc.x, 0.0)) - result.lambda;
    result.ddy = edgeWeights(e0, e1, e2, ndc + float2(0.0, pixelNdc.y)) - result.lambda;
    return result;
}

public float2 interpolate(Barycentrics bary, float2 a, float2 b, float2 c)
{
    return bary.lambda.x * a + bary.lambda.y * b + bary.lambda.z * c;
}

public float3 interpolate(Barycentrics bary, float3 a, float3 b, float3 c)
{
    return bary.lambda.x * a + bary.lambda.y * b + bary.lambda.z * c;
}

public float4 interpolate(Barycentrics bary, float4 a, float4 b, float4 c)
{
    return bary.lambda.x * a + bary.lambda.y * b + bary.lambda.z * c;
}

public float2 interpolateDdx(Barycentrics bary, float2 a, float2 b, float2 c)
{
    return bary.ddx.x * a + bary.ddx.y * b + bary.ddx.z * c;
}

public float2 interpolateDdy(Barycentrics bary, float2 a, float2 b, float2 c)
{
    return bary.ddy.x * a + bary.ddy.y * b + bary.ddy.z * c;
}
