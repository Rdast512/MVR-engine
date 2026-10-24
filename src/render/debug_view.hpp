#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

// what the main raster pass shows; values below Count match shaders/types.slang kDebugView*
enum class DebugView : uint32_t
{
    Lit = 0,
    Meshlets,
    Triangles,
    Primitives,
    NodeInstances,
    // reserved, not implemented yet
    LightingAlbedo,
    LightingNormals,
    LightingOnly,
    DlssInput,
    DlssMotionVectors,
    Count,
};

enum class DebugViewCategory : uint8_t
{
    Lit,
    Geometry,
    Lighting,
    Upscaling,
};

[[nodiscard]] constexpr const char* debugViewCategoryName(DebugViewCategory category)
{
    switch (category) {
    case DebugViewCategory::Lit:
        return "Lit";
    case DebugViewCategory::Geometry:
        return "Geometry";
    case DebugViewCategory::Lighting:
        return "Lighting";
    case DebugViewCategory::Upscaling:
        return "Upscaling";
    }
    return "";
}

struct DebugViewInfo
{
    const char* name;
    DebugViewCategory category;
    bool isImplemented;
};

// indexed by DebugView; drives the UI, shader selection and overlay label
inline constexpr std::array<DebugViewInfo, std::to_underlying(DebugView::Count)> kDebugViews = {{
    {"Lit", DebugViewCategory::Lit, true},
    {"Meshlets", DebugViewCategory::Geometry, true},
    {"Triangles", DebugViewCategory::Geometry, true},
    {"Primitives", DebugViewCategory::Geometry, true},
    {"Node instances", DebugViewCategory::Geometry, true},
    {"Albedo", DebugViewCategory::Lighting, false},
    {"Normals", DebugViewCategory::Lighting, false},
    {"Lighting only", DebugViewCategory::Lighting, false},
    {"DLSS input", DebugViewCategory::Upscaling, false},
    {"DLSS motion vectors", DebugViewCategory::Upscaling, false},
}};

[[nodiscard]] constexpr const DebugViewInfo& debugViewInfo(DebugView view)
{
    return kDebugViews[std::to_underlying(view)];
}

// linked mesh + fragment shader pair the main pass is drawn with
enum class RasterVariant : uint32_t
{
    Lit = 0,
    GeometryDebug,
    Count,
};

inline constexpr size_t kRasterVariantCount = std::to_underlying(RasterVariant::Count);

[[nodiscard]] constexpr RasterVariant rasterVariantFor(DebugView view)
{
    return debugViewInfo(view).category == DebugViewCategory::Geometry ? RasterVariant::GeometryDebug
                                                                       : RasterVariant::Lit;
}

struct DebugViewSettings
{
    // shaders/types.slang kDebugViewFlagEdges
    static constexpr uint32_t kFlagEdges = 1u << 8;

    DebugView view = DebugView::Lit;
    // barycentric triangle edges over geometry views
    bool showEdges = false;

    [[nodiscard]] RasterVariant rasterVariant() const { return rasterVariantFor(view); }

    // GpuFrameRoot::debugView: mode in the low byte, flags above
    [[nodiscard]] uint32_t packed() const
    {
        return std::to_underlying(view) | (showEdges ? kFlagEdges : 0u);
    }
};
