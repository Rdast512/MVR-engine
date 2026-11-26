module check_types;

// scalar layout: must match src/checks/check_types.hpp (layout_test); the check passes' GPU structs

// 16 B, matches src/checks/check_types.hpp
public struct MotionCheckPushData
{
    public uint64_t frameRoot;
    public uint64_t stats; // GpuMotionCheckStats
};

// 16 B, matches src/checks/check_types.hpp
public struct RayCheckPushData
{
    public uint64_t frameRoot;
    public uint64_t stats; // GpuRayCheckStats
};

// 16 B, matches src/checks/check_types.hpp
public struct SerCheckPushData
{
    public uint64_t frameRoot;
    public uint64_t stats; // GpuSerCheckStats of the pipeline's reorder mode
};

// 28 B, matches src/checks/check_types.hpp: rt_ser_check pass, ENGINE_RT_CHECK only. The SER trace against
// traceClosest on the same primary ray: both are the same traversal, so every field must match bit for bit
public struct GpuSerCheckStats
{
    public uint rays;
    public uint hits;           // traceClosest hits
    public uint hitMismatches;  // one of the two hits, the other misses
    public uint idMismatches;   // another (ray instance, meshlet, triangle)
    public uint dataMismatches; // same ids, other barycentrics, hit distance, positions or transforms
    public uint maxBarycentricErrorBits; // float bits
    public uint maxDistanceErrorBits;    // float bits, relative to the hit distance
};

// 72 B, matches src/checks/check_types.hpp: rt_check pass, ENGINE_RT_CHECK only. max* fields are float bits
public struct GpuRayCheckStats
{
    public uint coveredPixels;
    public uint interiorPixels; // covered, and the 4 neighbours hold the same payload
    public uint misses;         // interior, the primary ray hits nothing
    public uint idMismatches;   // interior, another (node instance, meshlet, triangle)
    public uint backFaceHits;   // the ray hits a back face of a one-sided primitive, which raster culls
    public uint edgeMismatches; // covered but not interior: another id along triangle edges
    public uint emptyHits;      // empty pixel with empty neighbours, the ray hits something
    public uint selfHits;       // a spawned ray from a matched hit hits its own triangle
    public uint mipOutliers;    // matched, ray-cone and raster uv footprints more than half a mip apart
    public uint depthTies;      // interior, another triangle within intersection noise of the raster one: coplanar
    // the decode at the resolve's barycentrics and gradients: any difference is the decode chain's
    public uint maxNormalErrorBits;         // degrees, Surface.normal
    public uint maxGeoNormalErrorBits;      // degrees
    public uint maxPositionErrorBits;       // pixels between the two intersections on screen
    public uint maxMipErrorBits;            // |log2| of the uv footprint ratio, uv set 0, ray cone vs raster
    public uint maxMaterialNormalErrorBits; // degrees
    // the same at the ray's own barycentrics: intersection noise through steep normal maps, reported only
    public uint maxHitNormalErrorBits;
    public uint maxHitMaterialNormalErrorBits;
    // a mismatch next to an alpha-tested triangle's cut, within one OMM micro-triangle plus the raster footprint
    public uint alphaEdges;
};

// 40 B, matches src/checks/check_types.hpp: motion_check pass, ENGINE_MOTION_TEST only
public struct GpuMotionCheckStats
{
    public uint pixels;
    public uint emptyPixels;
    public uint movingPixels;
    public uint cameraMismatches;
    public uint reprojectedErrors;
    public uint stillErrors;
    public uint offscreenPixels;
    public uint frameFlags;
    public uint maxMotionBits;      // float bits
    public uint maxCameraErrorBits; // float bits
};
