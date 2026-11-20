import types;
import core.binding;
import core.counters;
import core.frame;
import core.math;
import geometry.hit;
import geometry.surface;
import geometry.visbuffer;
import material.pbr;

// ray-tracing self-test, ENGINE_RT_CHECK only (plan C3): every pixel's primary ray against the vis buffer. Interior
// pixels must hit the same (node instance, meshlet, triangle), and the decode of the ray's ids and fetched positions
// at the resolve's barycentrics must rebuild the resolve's surface; a ray spawned from a matched hit must not hit its
// own triangle. Runs after the resolve; writes only GpuRayCheckStats

static const uint kRayCheckTile = 8; // matches src/render/vk_renderer.cpp
static const float kDegreesPerRadian = 180.0 / kPi;
// half a mip level between the ray-cone and raster uv footprints
static const float kMipTolerance = 0.5;
// one texel of a 65536^2 texture, in uv^2: below it both footprints pick mip 0. UV-degenerate triangles (all corners
// on one uv) sit far below, where both areas are float noise
static const float kMinUvFootprintArea = 1.0 / 4294967296.0;
// an edge moves at most as far as its farther corner snaps: subpixelSnap on each axis
static const float kSnapEdgeScale = 1.41421356;
static const float kNoEdge = 3.0e38;
// two triangles closer than this along the ray, over the hit distance, are a depth tie: coplanar overlap
static const float kDepthTieTolerance = 1e-4;

[[vk::push_constant]] ConstantBuffer<RayCheckPushData> push;

// exact near 0, unlike acos
float angleDegrees(float3 a, float3 b)
{
    return atan2(length(cross(a, b)), dot(a, b)) * kDegreesPerRadian;
}

// the 4 neighbours are covered alike and hold the same payload: no triangle edge crosses the pixel
bool isInteriorPixel(GpuTargets targets, int2 pixel, bool isCovered, uint payload)
{
    const int2 offsets[4] = { int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1) };
    [unroll]
    for (uint i = 0; i < 4; ++i)
    {
        const int2 neighbour = pixel + offsets[i];
        if (any(neighbour < 0) || any(neighbour >= int2(targets.renderSize)))
        {
            return false;
        }
        const bool isNeighbourCovered = tex2D(targets.depth).Load(int3(neighbour, 0)).r != 0.0;
        if (isNeighbourCovered != isCovered ||
            (isCovered && uintTex2D(targets.visBuffer).Load(int3(neighbour, 0)) != payload))
        {
            return false;
        }
    }
    return true;
}

// pixels from the center to the nearest edge of the triangle's projection; huge when a corner is behind the eye,
// where the projection does not bound the triangle
float edgeDistancePixels(TriangleCorners corners, float2 center, float2 targetSize)
{
    float2 screen[3];
    [unroll]
    for (uint i = 0; i < 3; ++i)
    {
        if (corners.clip[i].w <= 0.0)
        {
            return kNoEdge;
        }
        screen[i] = (corners.clip[i].xy / corners.clip[i].w * 0.5 + 0.5) * targetSize;
    }
    float distance = kNoEdge;
    [unroll]
    for (uint i = 0; i < 3; ++i)
    {
        const float2 edge = screen[(i + 1) % 3] - screen[i];
        const float2 toCenter = center - screen[i];
        distance = min(distance, abs(edge.x * toCenter.y - edge.y * toCenter.x) / max(length(edge), 1e-12));
    }
    return distance;
}

// raster and ray coverage may disagree only where the rasterizer's vertex snap can move an edge across the center
bool isOnSnappedEdge(GpuFrameRoot root, TriangleCorners corners, float2 center, float2 targetSize)
{
    return edgeDistancePixels(corners, center, targetSize) <= kSnapEdgeScale * root.subpixelSnap;
}

// pixels between two points' projections. A ray and the raster sample one point under the pixel; on a grazing
// surface a tiny direction error slides the hit far along the triangle, so world distance overstates the miss
float screenDistancePixels(GpuCameraData camera, float3 a, float3 b, float2 targetSize)
{
    const float4 clipA = mul(camera.viewProj, float4(a, 1.0));
    const float4 clipB = mul(camera.viewProj, float4(b, 1.0));
    return length((clipA.xy / clipA.w - clipB.xy / clipB.w) * 0.5 * targetSize);
}

// pcg
uint hashUint(uint value)
{
    const uint state = value * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// cosine-weighted around n: covers grazing directions, where spawn offsets fail first
float3 hemisphereDirection(float3 n, uint2 pixel)
{
    const uint first = hashUint(pixel.x | (pixel.y << 16));
    const float u1 = float(first >> 8) / 16777216.0;
    const float u2 = float(hashUint(first) >> 8) / 16777216.0;
    const float radius = sqrt(u1);
    const float phi = 2.0 * kPi * u2;
    float3 t;
    float3 b;
    buildOrthonormalBasis(n, t, b);
    return normalize(radius * cos(phi) * t + radius * sin(phi) * b + sqrt(max(1.0 - u1, 0.0)) * n);
}

bool isSameTriangle(RayHit a, RayHit b)
{
    return a.rayInstance == b.rayInstance && a.meshletRow == b.meshletRow && a.triangle == b.triangle;
}

// D1's bake settings, src/core/assets_loader.cpp kOmmTexelsPerMicroTriangleEdge / kOmmMaxSubdivisionLevel
static const float kOmmTexelsPerMicroTriangleEdge = 2.0;
static const uint kOmmMaxSubdivisionLevel = 5;
// texels between alpha samples: bilinear spreads a one-texel line over two
static const float kAlphaEdgeSampleSpacing = 1.0;
// per axis: wider disks (chains at subdivision 5) are sampled more sparsely
static const uint kMaxAlphaEdgeSteps = 64;

// the OMM SDK's dynamic subdivision (omm-lib bake_cpu_impl.cpp GetDynamicSubdivisionLevel): the level whose 4^N
// micro-triangles keep at least kOmmTexelsPerMicroTriangleEdge^2 texels each
uint ommSubdivisionLevel(float areaTexels)
{
    const uint ratio = uint(areaTexels / (kOmmTexelsPerMicroTriangleEdge * kOmmTexelsPerMicroTriangleEdge));
    const uint log2NextPow2 = ratio <= 1 ? 0 : firstbithigh(ratio - 1) + 1;
    return min(log2NextPow2 >> 1, kOmmMaxSubdivisionLevel);
}

// spec/m6/d4_ray_contract.md alpha-edge: the alpha test crosses its threshold within one OMM micro-triangle of the
// point, widened by the sampling footprint and one bilinear texel. There the OMM (per micro-triangle at mip 0, unknown
// as opaque) and the alpha bin (per texel at its mip) may disagree; away from it they may not
bool isNearAlphaCut(GpuFrameRoot root, DrawContext draw, TriangleCorners corners, Surface surface, float lodBias)
{
    const GpuMaterial material = loadMaterial(root, materialRow(draw.primitive));
    if (!isAlphaTested(material))
    {
        return false;
    }
    float2 uv[3];
    [unroll]
    for (uint i = 0; i < 3; ++i)
    {
        uv[i] = baseColorUv(material, corners.texCoord[i], loadVertexUv1(draw, corners.vertexIndex[i]));
    }
    const Texture2D texture = tex2D(NonUniformResourceIndex(material.baseColorTex));
    const SamplerState sampler = samplerOf(NonUniformResourceIndex(material.baseColorSamp));
    uint width;
    uint height;
    uint levels;
    texture.GetDimensions(0, width, height, levels);
    const float2 size = float2(width, height);
    // in texels; micro-triangles are similar to their triangle
    const float2 e0 = (uv[1] - uv[0]) * size;
    const float2 e1 = (uv[2] - uv[1]) * size;
    const float2 e2 = (uv[0] - uv[2]) * size;
    const float areaTexels = 0.5 * abs(e0.x * e2.y - e0.y * e2.x);
    const float longestEdge = max(length(e0), max(length(e1), length(e2)));
    const float microEdge = longestEdge / float(1u << ommSubdivisionLevel(areaTexels));
    const TextureGradients gradients = baseColorGradients(material, surface, lodBias);
    const float footprint = max(length(gradients.ddx * size), length(gradients.ddy * size));
    const float radius = microEdge + footprint + 1.0;

    // every texel of the disk: a sparse pattern misses one-texel lines. Only mismatching pixels get here
    const uint steps = min(uint(ceil(2.0 * radius / kAlphaEdgeSampleSpacing)) + 1, kMaxAlphaEdgeSteps);
    const float spacing = 2.0 * radius / float(max(steps - 1, 1u));
    const float threshold = alphaTestCutoff(material) / material.baseColorFactor.a;
    bool isBelow = false;
    bool isAbove = false;
    for (uint y = 0; y < steps; ++y)
    {
        for (uint x = 0; x < steps; ++x)
        {
            const float2 offset = float2(x, y) * spacing - radius;
            if (dot(offset, offset) > radius * radius)
            {
                continue;
            }
            const float alpha = texture.SampleLevel(sampler, gradients.uv + offset / size, 0.0).a;
            isBelow = isBelow || alpha < threshold;
            isAbove = isAbove || alpha >= threshold;
        }
    }
    return isBelow && isAbove;
}

[shader("compute")]
[numthreads(kRayCheckTile, kRayCheckTile, 1)]
void rayCheckMain(uint3 id : SV_DispatchThreadID)
{
    const GpuFrameRoot root = bdaPtr<GpuFrameRoot>(push.frameRoot)[0];
    const GpuTargets targets = loadTargets(root);
    if (any(id.xy >= targets.renderSize))
    {
        return;
    }
    GpuRayCheckStats* stats = bdaPtr<GpuRayCheckStats>(push.stats);
    const GpuCameraData camera = loadCamera(root);
    const float2 targetSize = float2(targets.renderSize);

    const bool isCovered = tex2D(targets.depth).Load(int3(id.xy, 0)).r != 0.0;
    const uint payload = isCovered ? uintTex2D(targets.visBuffer).Load(int3(id.xy, 0)) : 0u;
    const bool isInterior = isInteriorPixel(targets, int2(id.xy), isCovered, payload);

    const float2 center = float2(id.xy) + 0.5;
    const PrimaryRay primary = primaryRay(camera, id.xy, targetSize);
    RayHit hit;
    const bool isHit = traceClosest(root, primary.ray, hit);

    bool isMatched = false;
    bool isBackFace = false;
    // a coverage disagreement within the snap of a triangle edge: an edge pixel, not a mismatch
    bool isSnapEdge = false;
    bool isDepthTie = false;
    // a miss, id mismatch or empty hit next to an alpha-tested triangle's cut: logged, not a failure
    bool isAlphaEdge = false;
    bool isSelfHit = false;
    bool isMipOutlier = false;
    float normalError = 0.0;
    float geoNormalError = 0.0;
    float positionError = 0.0;
    float mipError = 0.0;
    float materialNormalError = 0.0;
    float hitNormalError = 0.0;
    float hitMaterialNormalError = 0.0;
    if (isHit)
    {
        const DecodedHit decoded = decodeHit(root, hit, primary.ray.Direction, primary.cone);
        isBackFace = !decoded.surface.isFrontFacing && !decoded.draw.isDoubleSided;
        const TriangleCorners hitTriangle = loadTriangleCorners(decoded.draw, camera, hit.triangle);
        if (isInterior && !isBackFace)
        {
            isSnapEdge = isOnSnappedEdge(root, hitTriangle, center, targetSize);
            if (!isCovered && !isSnapEdge)
            {
                isAlphaEdge = isNearAlphaCut(root, decoded.draw, hitTriangle, decoded.surface, camera.lodBias);
            }
        }
        if (isCovered)
        {
            const DrawContext draw = loadDrawContext(root, visPayloadRow(payload));
            const uint triangle = visPayloadTriangle(payload);
            isMatched = decoded.draw.visible.nodeInstanceIndex == draw.visible.nodeInstanceIndex &&
                        decoded.draw.visible.meshletRow == draw.visible.meshletRow && hit.triangle == triangle;
            if (!isMatched && isInterior && !isBackFace && !isSnapEdge)
            {
                const TriangleCorners corners = loadTriangleCorners(draw, camera, triangle);
                isSnapEdge = isOnSnappedEdge(root, corners, center, targetSize);
                // the raster triangle's point on this ray
                const Barycentrics bary = computeBarycentrics(corners.clip[0], corners.clip[1], corners.clip[2],
                                                              pixelToNdc(center, targetSize), 2.0 / targetSize);
                const Surface rasterSurface = buildSurface(draw, corners, bary);
                const float rasterT = length(rasterSurface.position - primary.ray.Origin);
                isDepthTie = !isSnapEdge && abs(hit.t - rasterT) <= kDepthTieTolerance * hit.t;
                if (!isSnapEdge && !isDepthTie)
                {
                    isAlphaEdge = isNearAlphaCut(root, draw, corners, rasterSurface, camera.lodBias) ||
                        isNearAlphaCut(root, decoded.draw, hitTriangle, decoded.surface, camera.lodBias);
                }
            }
            if (isMatched && isInterior)
            {
                // the resolve's surface at this pixel
                const TriangleCorners corners = loadTriangleCorners(draw, camera, triangle);
                const Barycentrics bary = computeBarycentrics(corners.clip[0], corners.clip[1], corners.clip[2],
                                                              pixelToNdc(center, targetSize),
                                                              2.0 / targetSize);
                const Surface raster = buildSurface(draw, corners, bary);
                const Surface traced = decoded.surface;
                positionError = screenDistancePixels(camera, raster.position, traced.position, targetSize);

                // the ray's ids, fetched positions and facing at the resolve's exact barycentrics and gradients: what
                // remains is the decode chain, not the intersection. Even a 1 - u - v ulp would cross the texture
                // unit's sub-texel steps on steep normal maps
                TriangleCorners hitCorners = loadCornerVertices(decoded.draw, hit.triangle);
                hitCorners.objectPos = hit.objectPos;
                hitCorners.isFrontFacing = traced.isFrontFacing;
                const Surface decodedAtRaster = buildSurface(decoded.draw, hitCorners, bary);
                normalError = angleDegrees(raster.normal, decodedAtRaster.normal);
                geoNormalError = angleDegrees(raster.geoNormal, decodedAtRaster.geoNormal);
                hitNormalError = angleDegrees(raster.normal, traced.normal);

                // footprint areas in uv set 0: |ddx x ddy| for raster, the cone's square for the ray
                const float rasterArea =
                    abs(raster.uvDdx[0].x * raster.uvDdy[0].y - raster.uvDdx[0].y * raster.uvDdy[0].x);
                const float coneArea = traced.uvDdx[0].x * traced.uvDdy[0].y;
                if (rasterArea > 0.0 && coneArea > 0.0 && max(rasterArea, coneArea) > kMinUvFootprintArea)
                {
                    mipError = abs(0.5 * log2(coneArea / rasterArea));
                    isMipOutlier = mipError > kMipTolerance;
                }

                // the ray's material row, through its own decode
                const GpuMaterial material = loadMaterial(root, materialRow(decoded.draw.primitive));
                const float3 rasterMaterialNormal =
                    sampleMaterial(loadMaterial(root, materialRow(draw.primitive)), raster, 0.0).n;
                materialNormalError =
                    angleDegrees(rasterMaterialNormal, sampleMaterial(material, decodedAtRaster, 0.0).n);
                // same gradients: the mip choice is checked above
                Surface tracedWithRasterGradients = traced;
                tracedWithRasterGradients.uvDdx = raster.uvDdx;
                tracedWithRasterGradients.uvDdy = raster.uvDdy;
                hitMaterialNormalError =
                    angleDegrees(rasterMaterialNormal, sampleMaterial(material, tracedWithRasterGradients, 0.0).n);

                RayDesc spawned;
                spawned.Direction = hemisphereDirection(traced.geoNormal, id.xy);
                spawned.Origin = safeSpawnPoint(hit, spawned.Direction);
                spawned.TMin = 0.0;
                spawned.TMax = camera.farZ;
                RayHit next;
                isSelfHit = traceClosest(root, spawned, next) && isSameTriangle(hit, next);
            }
        }
    }
    else if (isCovered && isInterior)
    {
        const DrawContext draw = loadDrawContext(root, visPayloadRow(payload));
        const TriangleCorners corners = loadTriangleCorners(draw, camera, visPayloadTriangle(payload));
        isSnapEdge = isOnSnappedEdge(root, corners, center, targetSize);
        if (!isSnapEdge)
        {
            const Barycentrics bary = computeBarycentrics(corners.clip[0], corners.clip[1], corners.clip[2],
                                                          pixelToNdc(center, targetSize), 2.0 / targetSize);
            isAlphaEdge = isNearAlphaCut(root, draw, corners, buildSurface(draw, corners, bary), camera.lodBias);
        }
    }

    waveCount(stats.coveredPixels, isCovered);
    waveCount(stats.interiorPixels, isCovered && isInterior);
    const bool isEdge = !isInterior || isSnapEdge;
    waveCount(stats.misses, isCovered && !isEdge && !isHit && !isAlphaEdge);
    waveCount(stats.idMismatches,
              isCovered && !isEdge && isHit && !isMatched && !isBackFace && !isDepthTie && !isAlphaEdge);
    waveCount(stats.depthTies, isDepthTie);
    waveCount(stats.backFaceHits, isHit && !isMatched && isBackFace);
    waveCount(stats.edgeMismatches, isCovered && isEdge && isHit && !isMatched && !isBackFace);
    waveCount(stats.emptyHits, !isCovered && !isEdge && isHit && !isBackFace && !isAlphaEdge);
    waveCount(stats.alphaEdges, isAlphaEdge);
    waveCount(stats.selfHits, isSelfHit);
    waveCount(stats.mipOutliers, isMipOutlier);
    waveMax(stats.maxNormalErrorBits, normalError);
    waveMax(stats.maxGeoNormalErrorBits, geoNormalError);
    waveMax(stats.maxPositionErrorBits, positionError);
    waveMax(stats.maxMipErrorBits, mipError);
    waveMax(stats.maxMaterialNormalErrorBits, materialNormalError);
    waveMax(stats.maxHitNormalErrorBits, hitNormalError);
    waveMax(stats.maxHitMaterialNormalErrorBits, hitMaterialNormalError);
}
