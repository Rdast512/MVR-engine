/*
 * SPDX-FileCopyrightText: Copyright (c) 2024-2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ported from nvpro-samples/vk_lod_clusters shaders/culling.glsl to Slang and modified for this engine:
 * - CULLING_REVERSED_Z switch (the original is reverse Z only); the engine runs reverse Z, tests run both
 * - the near-plane rule names the near-side clip bit for either depth convention
 * - clip bounds are accumulated per corner, so a world-space sphere can feed them without a world matrix
 * - mip choice and center fetch split out of intersectHiz (hizMip) so they can be unit-tested
 * - pixel epsilon from the device's subPixelPrecisionBits instead of a fixed 1/256; the pixel bbox holds pixel-center
 *   indices (floor/ceil) instead of rounded values, exact on half values
 * - isSubPixel: the pixel-bbox test applied to a box's clip rect (engine extension)
 * License text: ThirdParty/licenses/nvpro-samples-vk_lod_clusters-Apache-2.0.txt
 */
module culling;

import core.binding;

// frustum, Hi-Z, size and triangle tests shared by the cull passes and the mesh shaders. Bounds are boxes given as
// clip-space corners; the pyramid keeps the farthest depth of its footprint

#ifndef CULLING_REVERSED_Z
#define CULLING_REVERSED_Z 1 // must match the depth test (ShaderObjects::setRasterState) and the camera projection
#endif

#if CULLING_REVERSED_Z
public static const float kNearestDepth = 1.0;
#else
public static const float kNearestDepth = 0.0;
#endif

// the pyramid reduction (passes/hiz_build.slang) and the sampler reduction mode (HiZPyramid) follow it
public float farthestDepth(float a, float b)
{
#if CULLING_REVERSED_Z
    return min(a, b);
#else
    return max(a, b);
#endif
}

static const float kCullingEpsilon = 1.2e-07;
// two float ulps at depth 1 (D32_SFLOAT, 24-bit significand): absorbs the difference between the depth the
// rasterizer stored and the depth computed for a bound's corners
static const float kHiZDepthNudge = 2.0 / 16777216.0;

// getCullBits
static const uint kClipLeft = 1u;
static const uint kClipRight = 2u;
static const uint kClipBottom = 4u;
static const uint kClipTop = 8u;
static const uint kClipZBelow = 16u; // z < 0
static const uint kClipZAbove = 32u; // z > w
static const uint kClipBehindEye = 64u; // w <= 0
#if CULLING_REVERSED_Z
static const uint kClipNear = kClipZAbove;
#else
static const uint kClipNear = kClipZBelow;
#endif

uint getCullBits(float4 hPos)
{
    uint cullBits = 0;
    cullBits |= hPos.x < -hPos.w ? kClipLeft : 0u;
    cullBits |= hPos.x > hPos.w ? kClipRight : 0u;
    cullBits |= hPos.y < -hPos.w ? kClipBottom : 0u;
    cullBits |= hPos.y > hPos.w ? kClipTop : 0u;
    cullBits |= hPos.z < 0.0 ? kClipZBelow : 0u;
    cullBits |= hPos.z > hPos.w ? kClipZAbove : 0u;
    cullBits |= hPos.w <= 0.0 ? kClipBehindEye : 0u;
    return cullBits;
}

float4 getBoxCorner(float3 bboxMin, float3 bboxMax, uint n)
{
    const bool3 useMax = bool3((n & 1) != 0, (n & 2) != 0, (n & 4) != 0);
    return float4(select(useMax, bboxMax, bboxMin), 1.0);
}

// clip-space bounds of a box: xyz / |w| per corner, xy clamped to the screen after clipBoundsEnd
public struct ClipBounds
{
    public float4 clipMin;
    public float4 clipMax;
    // false: a corner sits on the eye plane, behind the eye or before the near plane. The rect and depth are then
    // meaningless and the box counts as visible for the Hi-Z, size and sub-pixel tests
    public bool isValid;
    public uint andBits; // all corners outside one plane: outside the frustum
    public uint orBits;
};

public ClipBounds clipBoundsBegin()
{
    ClipBounds bounds;
    bounds.clipMin = float4(3.402823466e+38);
    bounds.clipMax = float4(-3.402823466e+38);
    bounds.isValid = true;
    bounds.andBits = ~0u;
    bounds.orBits = 0u;
    return bounds;
}

void clipBoundsAdd(inout ClipBounds bounds, float4 hPos)
{
    const bool isValid = !(-kCullingEpsilon < hPos.w && hPos.w < kCullingEpsilon);
    const float4 clip = float4(hPos.xyz / abs(hPos.w), hPos.w);
    const uint bits = getCullBits(hPos);
    bounds.andBits &= bits;
    bounds.orBits |= bits;
    bounds.clipMin = min(bounds.clipMin, clip);
    bounds.clipMax = max(bounds.clipMax, clip);
    bounds.isValid = bounds.isValid && isValid;
}

// true: inside the frustum
bool clipBoundsEnd(inout ClipBounds bounds)
{
    bounds.isValid = bounds.isValid && (bounds.orBits & (kClipNear | kClipBehindEye)) == 0u;
    bounds.clipMin.xy = clamp(bounds.clipMin.xy, float2(-1.0), float2(1.0));
    bounds.clipMax.xy = clamp(bounds.clipMax.xy, float2(-1.0), float2(1.0));
    return bounds.andBits == 0u;
}

// object-space box under world
public bool intersectFrustum(float4x4 viewProj, float3 bboxMin, float3 bboxMax, float4x4 world, out ClipBounds bounds)
{
    const float4x4 worldViewProj = mul(viewProj, world);
    bounds = clipBoundsBegin();
    [unroll]
    for (uint n = 0; n < 8; ++n)
    {
        clipBoundsAdd(bounds, mul(worldViewProj, getBoxCorner(bboxMin, bboxMax, n)));
    }
    return clipBoundsEnd(bounds);
}

// world-space AABB of a world-space sphere; clip(center + r * e_i) = clip(center) + r * column i of viewProj
public bool intersectFrustumSphere(float4x4 viewProj, float3 center, float radius, out ClipBounds bounds)
{
    const float4 clipCenter = mul(viewProj, float4(center, 1.0));
    const float4 dx = radius * float4(viewProj[0][0], viewProj[1][0], viewProj[2][0], viewProj[3][0]);
    const float4 dy = radius * float4(viewProj[0][1], viewProj[1][1], viewProj[2][1], viewProj[3][1]);
    const float4 dz = radius * float4(viewProj[0][2], viewProj[1][2], viewProj[2][2], viewProj[3][2]);
    bounds = clipBoundsBegin();
    [unroll]
    for (uint n = 0; n < 8; ++n)
    {
        clipBoundsAdd(bounds, clipCenter + ((n & 1) != 0 ? dx : -dx) + ((n & 2) != 0 ? dy : -dy) +
                                  ((n & 4) != 0 ? dz : -dz));
    }
    return clipBoundsEnd(bounds);
}

// frustum test without the clip-space bounds, saves the per-corner divide. Used to drop items that cannot reappear
// in the second cull pass
bool intersectFrustumOnly(float4x4 viewProj, float3 bboxMin, float3 bboxMax, float4x4 world)
{
    const float4x4 worldViewProj = mul(viewProj, world);
    uint andBits = ~0u;
    [unroll]
    for (uint n = 0; n < 8; ++n)
    {
        andBits &= getCullBits(mul(worldViewProj, getBoxCorner(bboxMin, bboxMax, n)));
    }
    return andBits == 0u;
}

// screen rect in [0, 1], y down like the framebuffer
public float4 clipUvRect(ClipBounds bounds)
{
    return float4(bounds.clipMin.xy, bounds.clipMax.xy) * 0.5 + 0.5;
}

public float clipNearestDepth(ClipBounds bounds)
{
#if CULLING_REVERSED_Z
    return bounds.clipMax.z;
#else
    return bounds.clipMin.z;
#endif
}

// mip where the rect spans at most 1 texel, so the 2x2 footprint of one fetch at its center covers it; one finer when
// the rect fits a 2x2 block there without straddling (niagara acb8c83). Mips stop halving at 1 texel
public float hizMip(float4 uvRect, uint2 hizSize)
{
    const float2 uvSize = uvRect.zw - uvRect.xy;
    const float2 extent = uvSize * float2(hizSize);
    // log2(0) = -inf clamps to 0
    float mip = max(ceil(log2(max(extent.x, extent.y))), 0.0);
    const float2 fineMipSize = max(float2(hizSize) * exp2(1.0 - mip), 1.0);
    if (mip > 0.0 && all(frac(uvRect.xy * fineMipSize) + uvSize * fineMipSize <= 2.0))
    {
        mip -= 1.0;
    }
    return mip;
}

// intersectHiz: one far-reduction fetch at the rect center. Samples past the last mip clamp to the 1x1 level, which
// covers the whole screen. An invalid box is never occluded
public bool isHiZOccluded(ClipBounds bounds, uint2 hizSize, uint pyramidIndex, uint farSamplerIndex)
{
    if (!bounds.isValid)
    {
        return false;
    }
    const float4 uvRect = clipUvRect(bounds);
    const float2 center = (uvRect.xy + uvRect.zw) * 0.5;
    const float farthest =
        tex2D(pyramidIndex).SampleLevel(samplerOf(farSamplerIndex), center, hizMip(uvRect, hizSize)).r;
#if CULLING_REVERSED_Z
    return clipNearestDepth(bounds) < farthest - kHiZDepthNudge;
#else
    return clipNearestDepth(bounds) > farthest + kHiZDepthNudge;
#endif
}

////////////////////////////////////////////////////////////////////////////
// pixel and triangle tests

public float2 getScreenPos(float4 hPos, float2 viewport)
{
    return ((hPos.xy / hPos.w) * 0.5 + 0.5) * viewport;
}

// widen by how far the rasterizer may move a vertex when snapping it to the subpixel grid, then turn the bbox into
// the first and last pixel-center index it covers (centers sit at k + 0.5). The original rounds and compares
// min == max; floor/ceil gives the same answer except on exact half values, where round() is not exact
void pixelBboxEpsilon(inout float2 pixelMin, inout float2 pixelMax, float subpixelSnap)
{
    pixelMin = ceil(pixelMin - subpixelSnap - 0.5);
    pixelMax = floor(pixelMax + subpixelSnap - 0.5);
}

// no pixel center inside on one axis
bool pixelBboxCull(float2 pixelMin, float2 pixelMax)
{
    return any(pixelMax < pixelMin);
}

bool pixelViewportCull(float2 pixelMin, float2 pixelMax, float2 viewport)
{
    return pixelMax.x < 0.0 || pixelMin.x >= viewport.x || pixelMax.y < 0.0 || pixelMin.y >= viewport.y;
}

// true: the triangle may cover a pixel center
public bool testTrianglePixel(float2 a, float2 b, float2 c, bool isFrustum, float2 viewport, float subpixelSnap)
{
    float2 pixelMin = min(a, min(b, c));
    float2 pixelMax = max(a, max(b, c));
    pixelBboxEpsilon(pixelMin, pixelMax, subpixelSnap);
    // the cull passes already did the frustum test at meshlet level
    if (isFrustum && pixelViewportCull(pixelMin, pixelMax, viewport))
    {
        return false;
    }
    return !pixelBboxCull(pixelMin, pixelMax);
}

// homogeneous orientation (Olano-Greer; https://zeux.io/2023/04/28/triangle-backface-culling/): no divide, valid for
// triangles crossing the eye plane. With proj[1][1] flipped, a positive viewport height and frontFace = CCW, front
// faces are <= 0
public bool isFrontFacingHW(float4 ha, float4 hb, float4 hc)
{
    return determinant(float3x3(ha.xyw, hb.xyw, hc.xyw)) <= 0.0;
}

// engine extension: the pixel-bbox test on a box's rect. Lossless, since the rect is conservative: no pixel center
// inside on one axis means nothing in the box can produce a fragment. Invalid boxes are never sub-pixel
public bool isSubPixel(ClipBounds bounds, float2 viewport, float subpixelSnap)
{
    if (!bounds.isValid)
    {
        return false;
    }
    const float4 uvRect = clipUvRect(bounds);
    float2 pixelMin = uvRect.xy * viewport;
    float2 pixelMax = uvRect.zw * viewport;
    pixelBboxEpsilon(pixelMin, pixelMax, subpixelSnap);
    return pixelBboxCull(pixelMin, pixelMax);
}
