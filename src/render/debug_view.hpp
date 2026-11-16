#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

// what passes/debug_view.slang writes to the display image; values below Count match shaders/types.slang kDebugView*
enum class DebugView : uint32_t
{
    // until lighting (M3): the diffuse albedo guide
    Lit = 0,
    Meshlets,
    Triangles,
    Primitives,
    NodeInstances,
    // material flags of the primitive: two-sided, and the raster bin
    Bins,
    // early phase blue, late phase orange
    CullPhase,
    // fullscreen pyramid mip of the final depth (passes/hiz_view.slang over the present pass)
    HiZPyramid,
    Depth,
    ShadingNormal,
    Roughness,
    GeometricNormal,
    DiffuseAlbedo,
    SpecularF0,
    SpecularAlbedo,
    Emissive,
    MotionVectors,
    // albedo against last frame's at the reprojected position: only disocclusions light up
    Reprojection,
    // primary rays through the TLAS, decoded like a vis-buffer pixel: must match Meshlets / NodeInstances
    RayMeshlets,
    RayNodeInstances,
    // material normal of the decoded hit, textures sampled at ray-cone gradients
    RayShadingNormal,
    // DiffuseAlbedo.a: the occlusion texture with its strength
    AmbientOcclusion,
    // every world light on the guides with no visibility test: checks the light buffer before the lighting passes
    UnshadowedLights,
    // every world light through the RTXDI bridge (RAB_*) with a visibility ray each: the reference ReSTIR DI converges to
    ShadowedLights,
    // ReSTIR DI directRadiance + the emissive guide: the lighting output until the combine pass (M4)
    LightingOnly,
    // passes/combine.slang's noisyColor: what DLSS-RR receives as color, at render resolution
    DlssInput,
    // the reservoir rtxdi_shade stored: hue by light, brightness by its sample count M up to the history cap
    RtxdiReservoirs,
    // the indirect pass's initial ReSTIR GI reservoir shaded without reuse: one-bounce indirect light, 1 sample
    IndirectSamples,
    Count,
};

// shaders/types.slang kDebugViewRayMeshlets
static_assert(std::to_underlying(DebugView::RayMeshlets) == 18);
// shaders/types.slang kDebugViewAmbientOcclusion
static_assert(std::to_underlying(DebugView::AmbientOcclusion) == 21);
static_assert(std::to_underlying(DebugView::UnshadowedLights) == 22);
static_assert(std::to_underlying(DebugView::ShadowedLights) == 23);
static_assert(std::to_underlying(DebugView::LightingOnly) == 24);
static_assert(std::to_underlying(DebugView::DlssInput) == 25);
static_assert(std::to_underlying(DebugView::RtxdiReservoirs) == 26);
static_assert(std::to_underlying(DebugView::IndirectSamples) == 27);

enum class DebugViewCategory : uint8_t
{
    Lit,
    Geometry,
    Culling,
    Guides,
    RayTracing,
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
    case DebugViewCategory::Culling:
        return "Culling";
    case DebugViewCategory::Guides:
        return "Guides";
    case DebugViewCategory::RayTracing:
        return "Ray tracing";
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

// indexed by DebugView; drives the UI and the overlay label
inline constexpr std::array<DebugViewInfo, std::to_underlying(DebugView::Count)> kDebugViews = {{
    {"Lit", DebugViewCategory::Lit, true},
    {"Meshlets", DebugViewCategory::Geometry, true},
    {"Triangles", DebugViewCategory::Geometry, true},
    {"Primitives", DebugViewCategory::Geometry, true},
    {"Node instances", DebugViewCategory::Geometry, true},
    {"Bins", DebugViewCategory::Geometry, true},
    {"Cull phase", DebugViewCategory::Culling, true},
    {"Hi-Z pyramid", DebugViewCategory::Culling, true},
    {"Depth", DebugViewCategory::Guides, true},
    {"Shading normal", DebugViewCategory::Guides, true},
    {"Roughness", DebugViewCategory::Guides, true},
    {"Geometric normal", DebugViewCategory::Guides, true},
    {"Diffuse albedo", DebugViewCategory::Guides, true},
    {"Specular F0", DebugViewCategory::Guides, true},
    {"Specular albedo", DebugViewCategory::Guides, true},
    {"Emissive", DebugViewCategory::Guides, true},
    {"Motion vectors", DebugViewCategory::Guides, true},
    {"Reprojection", DebugViewCategory::Guides, true},
    {"Ray meshlets", DebugViewCategory::RayTracing, true},
    {"Ray node instances", DebugViewCategory::RayTracing, true},
    {"Ray shading normal", DebugViewCategory::RayTracing, true},
    {"Ambient occlusion", DebugViewCategory::Guides, true},
    {"Unshadowed lights", DebugViewCategory::Lighting, true},
    {"Shadowed lights", DebugViewCategory::Lighting, true},
    {"Lighting only", DebugViewCategory::Lighting, true},
    {"DLSS input", DebugViewCategory::Upscaling, true},
    {"RTXDI reservoirs", DebugViewCategory::Lighting, true},
    {"Indirect samples", DebugViewCategory::Lighting, true},
}};

[[nodiscard]] constexpr const DebugViewInfo& debugViewInfo(DebugView view)
{
    return kDebugViews[std::to_underlying(view)];
}

struct DebugViewSettings
{
    // shaders/types.slang kDebugViewFlagEdges
    static constexpr uint32_t kFlagEdges = 1u << 8;

    DebugView view = DebugView::Lit;
    // analytic-barycentric triangle edges over every view
    bool showEdges = false;
    // DebugView::HiZPyramid; clamped to the last mip
    uint32_t hizMip = 0;

    // GpuFrameRoot::debugView: mode in the low byte, flags above
    [[nodiscard]] uint32_t packed() const
    {
        return std::to_underlying(view) | (showEdges ? kFlagEdges : 0u);
    }
};
