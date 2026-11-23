// Compares the scalar layout Slang reflects for every struct in shaders/types.slang, for GpuRtxdiConstants in
// shaders/lighting/rtxdi_constants.slang and for GpuNrcConstants in shaders/gi/nrc_constants.slang, with sizeof /
// offsetof of its C++ mirror. A Slang struct without a mirror, a field missing on either side, or a size or offset that differs
// fails the test. A field the C++ side renames fails to compile (offsetof). Also checks the storage-image format
// constants of shaders/core/binding.slang against SlangImageFormat.
// --dump prints the reflected layouts instead of checking them.
#include "pch.h"

#include "core/types.hpp"
#include "render/push_data.hpp"
#include "render/vk_nrc.hpp"
#include "render/vk_rtxdi.hpp"
#include "test_support.hpp"

#include <slang-com-ptr.h>
#include <slang.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using test::check;

namespace
{
    struct FieldLayout
    {
        std::string_view name;
        size_t offset;
    };

    struct StructLayout
    {
        std::string_view name;
        size_t size;
        std::vector<FieldLayout> fields;
    };

    // shader-only structs: no C++ mirror
    constexpr std::array<std::string_view, 2> kShaderOnly = {"MeshVertexOut", "MeshAlphaVertexOut"};

#define LAYOUT_FIELD(type, field)                                                                                      \
    FieldLayout { #field, offsetof(type, field) }
// the Slang name differs from the C++ member
#define LAYOUT_FIELD_AS(type, slangName, cppMember)                                                                    \
    FieldLayout { #slangName, offsetof(type, cppMember) }

    // clang-format off
    const std::vector<StructLayout> kMirrors = {
        {"GpuCameraData", sizeof(GpuCameraData), {
            LAYOUT_FIELD(GpuCameraData, view),
            LAYOUT_FIELD(GpuCameraData, proj),
            LAYOUT_FIELD(GpuCameraData, viewProj),
            LAYOUT_FIELD(GpuCameraData, invView),
            LAYOUT_FIELD(GpuCameraData, invProj),
            LAYOUT_FIELD(GpuCameraData, invViewProj),
            LAYOUT_FIELD(GpuCameraData, prevViewProj),
            LAYOUT_FIELD(GpuCameraData, cameraPos),
            LAYOUT_FIELD(GpuCameraData, nearZ),
            LAYOUT_FIELD(GpuCameraData, renderTargetSize),
            LAYOUT_FIELD(GpuCameraData, invRenderTargetSize),
            LAYOUT_FIELD(GpuCameraData, jitterOffset),
            LAYOUT_FIELD(GpuCameraData, farZ),
            LAYOUT_FIELD(GpuCameraData, frameDeltaTime),
            LAYOUT_FIELD(GpuCameraData, prevCameraPos),
            LAYOUT_FIELD(GpuCameraData, lodBias),
            LAYOUT_FIELD(GpuCameraData, rayOrigin),
            LAYOUT_FIELD(GpuCameraData, rayDx),
            LAYOUT_FIELD(GpuCameraData, rayDy),
            LAYOUT_FIELD(GpuCameraData, prevRayOrigin),
            LAYOUT_FIELD(GpuCameraData, prevRayDx),
            LAYOUT_FIELD(GpuCameraData, prevRayDy),
            LAYOUT_FIELD(GpuCameraData, rayPad0),
            LAYOUT_FIELD(GpuCameraData, rayPad1),
        }},
        {"GpuObjectUB", sizeof(GpuObjectUB), {
            LAYOUT_FIELD(GpuObjectUB, modelMatrix),
            LAYOUT_FIELD(GpuObjectUB, prevModelMatrix),
            LAYOUT_FIELD(GpuObjectUB, boundingSphere),
            LAYOUT_FIELD(GpuObjectUB, modelIndex),
            LAYOUT_FIELD(GpuObjectUB, instanceFlags),
            LAYOUT_FIELD(GpuObjectUB, pad0),
            LAYOUT_FIELD(GpuObjectUB, pad1),
        }},
        {"GpuModel", sizeof(GpuModel), {
            LAYOUT_FIELD(GpuModel, vertices),
            LAYOUT_FIELD(GpuModel, meshlets),
            LAYOUT_FIELD(GpuModel, meshletBounds),
            LAYOUT_FIELD(GpuModel, meshletGroups),
            LAYOUT_FIELD(GpuModel, meshletPrimitives),
            LAYOUT_FIELD(GpuModel, meshletVertices),
            LAYOUT_FIELD(GpuModel, meshletTriangles),
            LAYOUT_FIELD(GpuModel, normals),
            LAYOUT_FIELD(GpuModel, tangents),
            LAYOUT_FIELD(GpuModel, uv1),
            LAYOUT_FIELD(GpuModel, indices),
            LAYOUT_FIELD(GpuModel, nodes),
            LAYOUT_FIELD(GpuModel, primitives),
            LAYOUT_FIELD(GpuModel, attributeMask),
            LAYOUT_FIELD(GpuModel, nodeCount),
        }},
        {"GpuNode", sizeof(GpuNode), {
            LAYOUT_FIELD(GpuNode, localToModel),
            LAYOUT_FIELD(GpuNode, boundingSphere),
            LAYOUT_FIELD(GpuNode, firstPrimitive),
            LAYOUT_FIELD(GpuNode, primitiveCount),
            LAYOUT_FIELD(GpuNode, flags),
            LAYOUT_FIELD(GpuNode, pad0),
            LAYOUT_FIELD(GpuNode, bboxMin),
            LAYOUT_FIELD(GpuNode, pad1),
            LAYOUT_FIELD(GpuNode, bboxMax),
            LAYOUT_FIELD(GpuNode, pad2),
        }},
        {"GpuNodeInstance", sizeof(GpuNodeInstance), {
            LAYOUT_FIELD(GpuNodeInstance, worldMatrix),
            LAYOUT_FIELD(GpuNodeInstance, prevWorldMatrix),
            LAYOUT_FIELD(GpuNodeInstance, entityIndex),
            LAYOUT_FIELD(GpuNodeInstance, modelIndex),
            LAYOUT_FIELD(GpuNodeInstance, nodeIndex),
            LAYOUT_FIELD(GpuNodeInstance, flags),
        }},
        {"GpuVisibleMeshlet", sizeof(GpuVisibleMeshlet), {
            LAYOUT_FIELD(GpuVisibleMeshlet, nodeInstanceIndex),
            LAYOUT_FIELD(GpuVisibleMeshlet, meshletRow),
        }},
        {"GpuDrawRecord", sizeof(GpuDrawRecord), {
            LAYOUT_FIELD(GpuDrawRecord, nodeInstanceIndex),
            LAYOUT_FIELD(GpuDrawRecord, primitiveIndex),
            LAYOUT_FIELD(GpuDrawRecord, meshletOffset),
            LAYOUT_FIELD(GpuDrawRecord, flags),
            LAYOUT_FIELD(GpuDrawRecord, groupOffset),
        }},
        {"GpuInstanceCullItem", sizeof(GpuInstanceCullItem), {
            LAYOUT_FIELD(GpuInstanceCullItem, nodeInstanceIndex),
            LAYOUT_FIELD(GpuInstanceCullItem, firstDrawRecord),
            LAYOUT_FIELD(GpuInstanceCullItem, drawRecordCount),
        }},
        {"GpuCullView", sizeof(GpuCullView), {
            LAYOUT_FIELD(GpuCullView, cameraPos),
            LAYOUT_FIELD(GpuCullView, flags),
            LAYOUT_FIELD(GpuCullView, visibleMeshlets),
            LAYOUT_FIELD(GpuCullView, counters),
            LAYOUT_FIELD(GpuCullView, sequences),
            LAYOUT_FIELD(GpuCullView, instanceItems),
            LAYOUT_FIELD(GpuCullView, drawRecordTemplates),
            LAYOUT_FIELD(GpuCullView, instanceCount),
            LAYOUT_FIELD(GpuCullView, maxMeshletsPerSequence),
            LAYOUT_FIELD(GpuCullView, binVisibleOffset),
            LAYOUT_FIELD(GpuCullView, binSequenceOffset),
            LAYOUT_FIELD(GpuCullView, occlusionViewProj),
            LAYOUT_FIELD(GpuCullView, hizPyramid),
            LAYOUT_FIELD(GpuCullView, hizFarSampler),
            LAYOUT_FIELD(GpuCullView, hizSize),
            LAYOUT_FIELD(GpuCullView, instanceCandidates),
            LAYOUT_FIELD(GpuCullView, lateCounters),
            LAYOUT_FIELD(GpuCullView, drawRecordBase),
            LAYOUT_FIELD(GpuCullView, rejectRecordBase),
            LAYOUT_FIELD(GpuCullView, rejectMasks),
            LAYOUT_FIELD(GpuCullView, cullViewProj),
            LAYOUT_FIELD(GpuCullView, targetSize),
            LAYOUT_FIELD(GpuCullView, subpixelSnap),
            LAYOUT_FIELD(GpuCullView, instanceCandidateCapacity),
            LAYOUT_FIELD(GpuCullView, drawRecordCapacity),
            LAYOUT_FIELD(GpuCullView, binVisibleCapacity),
            LAYOUT_FIELD(GpuCullView, pad1),
        }},
        {"GpuGlassCounters", sizeof(GpuGlassCounters), {
            LAYOUT_FIELD(GpuGlassCounters, pixels),
            LAYOUT_FIELD(GpuGlassCounters, earlyStops),
            LAYOUT_FIELD(GpuGlassCounters, capped),
            LAYOUT_FIELD(GpuGlassCounters, pad0),
            LAYOUT_FIELD(GpuGlassCounters, layerPixels),
        }},
        {"GpuCullCounters", sizeof(GpuCullCounters), {
            LAYOUT_FIELD(GpuCullCounters, visibleMeshlets),
            LAYOUT_FIELD(GpuCullCounters, sequenceCount),
            LAYOUT_FIELD(GpuCullCounters, meshletDispatch),
            LAYOUT_FIELD(GpuCullCounters, instanceDispatch),
            LAYOUT_FIELD(GpuCullCounters, meshletsRetested),
            LAYOUT_FIELD(GpuCullCounters, instancesCulled),
            LAYOUT_FIELD(GpuCullCounters, instancesDrawn),
            LAYOUT_FIELD(GpuCullCounters, instancesOccluded),
            LAYOUT_FIELD(GpuCullCounters, testedMeshlets),
            LAYOUT_FIELD(GpuCullCounters, frustumCulled),
            LAYOUT_FIELD(GpuCullCounters, coneCulled),
            LAYOUT_FIELD(GpuCullCounters, occlusionCulled),
            LAYOUT_FIELD(GpuCullCounters, trianglesTested),
            LAYOUT_FIELD(GpuCullCounters, trianglesBackface),
            LAYOUT_FIELD(GpuCullCounters, trianglesSmall),
            LAYOUT_FIELD(GpuCullCounters, instancesSubPixel),
            LAYOUT_FIELD(GpuCullCounters, meshletsSubPixel),
            LAYOUT_FIELD(GpuCullCounters, groupsTested),
            LAYOUT_FIELD(GpuCullCounters, groupsCulled),
            LAYOUT_FIELD(GpuCullCounters, candidatesOverflowed),
            LAYOUT_FIELD(GpuCullCounters, drawRecordsOverflowed),
            LAYOUT_FIELD(GpuCullCounters, visibleOverflowed),
        }},
        {"GpuPrimitive", sizeof(GpuPrimitive), {
            LAYOUT_FIELD(GpuPrimitive, firstMeshlet),
            LAYOUT_FIELD(GpuPrimitive, meshletCount),
            LAYOUT_FIELD(GpuPrimitive, materialId),
            LAYOUT_FIELD(GpuPrimitive, firstVertex),
            LAYOUT_FIELD(GpuPrimitive, vertexCount),
            LAYOUT_FIELD(GpuPrimitive, firstIndex),
            LAYOUT_FIELD(GpuPrimitive, indexCount),
            LAYOUT_FIELD(GpuPrimitive, firstGroup),
            LAYOUT_FIELD(GpuPrimitive, groupCount),
            LAYOUT_FIELD(GpuPrimitive, flags),
        }},
        {"GpuVertex", sizeof(GpuVertex), {
            LAYOUT_FIELD(GpuVertex, pos),
            LAYOUT_FIELD(GpuVertex, color),
            LAYOUT_FIELD(GpuVertex, texCoord),
        }},
        {"GpuMeshletDesc", sizeof(GpuMeshletDesc), {
            LAYOUT_FIELD(GpuMeshletDesc, vertexOffset),
            LAYOUT_FIELD(GpuMeshletDesc, triangleOffset),
            LAYOUT_FIELD(GpuMeshletDesc, vertexCount),
            LAYOUT_FIELD(GpuMeshletDesc, triangleCount),
            LAYOUT_FIELD(GpuMeshletDesc, pad0),
            LAYOUT_FIELD(GpuMeshletDesc, pad1),
            FieldLayout{"coneAxisX", offsetof(GpuMeshletDesc, coneAxis) + 0},
            FieldLayout{"coneAxisY", offsetof(GpuMeshletDesc, coneAxis) + 1},
            FieldLayout{"coneAxisZ", offsetof(GpuMeshletDesc, coneAxis) + 2},
            LAYOUT_FIELD(GpuMeshletDesc, coneCutoff),
            LAYOUT_FIELD(GpuMeshletDesc, boundingSphere),
        }},
        {"GpuMeshletGroup", sizeof(GpuMeshletGroup), {
            LAYOUT_FIELD(GpuMeshletGroup, bboxMin),
            LAYOUT_FIELD(GpuMeshletGroup, firstMeshlet),
            LAYOUT_FIELD(GpuMeshletGroup, bboxMax),
            LAYOUT_FIELD(GpuMeshletGroup, meshletCount),
        }},
        {"GpuMeshletBounds", sizeof(GpuMeshletBounds), {
            LAYOUT_FIELD(GpuMeshletBounds, bboxMin),
            LAYOUT_FIELD(GpuMeshletBounds, shortestEdge),
            LAYOUT_FIELD(GpuMeshletBounds, bboxMax),
            LAYOUT_FIELD(GpuMeshletBounds, longestEdge),
        }},
        {"GpuUvMatrix", sizeof(GpuUvMatrix), {
            LAYOUT_FIELD(GpuUvMatrix, row0),
            LAYOUT_FIELD(GpuUvMatrix, row1),
        }},
        {"GpuMaterial", sizeof(GpuMaterial), {
            LAYOUT_FIELD(GpuMaterial, baseColorFactor),
            LAYOUT_FIELD(GpuMaterial, emissiveFactor),
            LAYOUT_FIELD(GpuMaterial, metallicFactor),
            LAYOUT_FIELD(GpuMaterial, roughnessFactor),
            LAYOUT_FIELD(GpuMaterial, normalScale),
            LAYOUT_FIELD(GpuMaterial, occlusionStrength),
            LAYOUT_FIELD(GpuMaterial, alphaCutoff),
            LAYOUT_FIELD(GpuMaterial, flags),
            LAYOUT_FIELD(GpuMaterial, baseColorTex),
            LAYOUT_FIELD(GpuMaterial, metalRoughTex),
            LAYOUT_FIELD(GpuMaterial, normalTex),
            LAYOUT_FIELD(GpuMaterial, occlusionTex),
            LAYOUT_FIELD(GpuMaterial, emissiveTex),
            LAYOUT_FIELD(GpuMaterial, baseColorSamp),
            LAYOUT_FIELD(GpuMaterial, metalRoughSamp),
            LAYOUT_FIELD(GpuMaterial, normalSamp),
            LAYOUT_FIELD(GpuMaterial, occlusionSamp),
            LAYOUT_FIELD(GpuMaterial, emissiveSamp),
            LAYOUT_FIELD(GpuMaterial, baseColorUv),
            LAYOUT_FIELD(GpuMaterial, metalRoughUv),
            LAYOUT_FIELD(GpuMaterial, normalUv),
            LAYOUT_FIELD(GpuMaterial, occlusionUv),
            LAYOUT_FIELD(GpuMaterial, emissiveUv),
            LAYOUT_FIELD(GpuMaterial, pad0),
            LAYOUT_FIELD(GpuMaterial, pad1),
            LAYOUT_FIELD(GpuMaterial, pad2),
            LAYOUT_FIELD(GpuMaterial, specularFactor),
            LAYOUT_FIELD(GpuMaterial, uvMatrices),
            LAYOUT_FIELD(GpuMaterial, glossinessFactor),
            LAYOUT_FIELD(GpuMaterial, pad4),
        }},
        {"GpuLight", sizeof(GpuLight), {
            LAYOUT_FIELD(GpuLight, position),
            LAYOUT_FIELD(GpuLight, range),
            LAYOUT_FIELD(GpuLight, direction),
            LAYOUT_FIELD(GpuLight, intensity),
            LAYOUT_FIELD(GpuLight, color),
            LAYOUT_FIELD(GpuLight, type),
            LAYOUT_FIELD(GpuLight, innerCone),
            LAYOUT_FIELD(GpuLight, outerCone),
            LAYOUT_FIELD(GpuLight, instanceIndex),
            LAYOUT_FIELD(GpuLight, pad0),
        }},
        {"GpuTriangleLight", sizeof(GpuTriangleLight), {
            LAYOUT_FIELD(GpuTriangleLight, v0),
            LAYOUT_FIELD(GpuTriangleLight, edge1),
            LAYOUT_FIELD(GpuTriangleLight, edge2),
            LAYOUT_FIELD(GpuTriangleLight, flags),
            LAYOUT_FIELD(GpuTriangleLight, pad0),
            LAYOUT_FIELD(GpuTriangleLight, type),
            LAYOUT_FIELD(GpuTriangleLight, radiance),
            LAYOUT_FIELD(GpuTriangleLight, pad1),
        }},
        {"GpuUvTransform", sizeof(GpuUvTransform), {
            LAYOUT_FIELD(GpuUvTransform, offset),
            LAYOUT_FIELD(GpuUvTransform, scale),
            LAYOUT_FIELD(GpuUvTransform, rotation),
        }},
        {"GpuTextureRef", sizeof(GpuTextureRef), {
            LAYOUT_FIELD(GpuTextureRef, tex),
            LAYOUT_FIELD(GpuTextureRef, samp),
            LAYOUT_FIELD(GpuTextureRef, uv),
            LAYOUT_FIELD(GpuTextureRef, normalScale),
            LAYOUT_FIELD(GpuTextureRef, uvXform),
        }},
        {"GpuMaterialExtension", sizeof(GpuMaterialExtension), {
            LAYOUT_FIELD(GpuMaterialExtension, flags),
            LAYOUT_FIELD(GpuMaterialExtension, baseColorUv),
            LAYOUT_FIELD(GpuMaterialExtension, metalRoughUv),
            LAYOUT_FIELD(GpuMaterialExtension, normalUv),
            LAYOUT_FIELD(GpuMaterialExtension, occlusionUv),
            LAYOUT_FIELD(GpuMaterialExtension, emissiveUv),
            LAYOUT_FIELD(GpuMaterialExtension, ior),
            LAYOUT_FIELD(GpuMaterialExtension, emissiveStrength),
            LAYOUT_FIELD(GpuMaterialExtension, dispersion),
            LAYOUT_FIELD(GpuMaterialExtension, specularFactor),
            LAYOUT_FIELD(GpuMaterialExtension, specularColorFactor),
            LAYOUT_FIELD(GpuMaterialExtension, specular),
            LAYOUT_FIELD(GpuMaterialExtension, specularColor),
            LAYOUT_FIELD(GpuMaterialExtension, clearcoatFactor),
            LAYOUT_FIELD(GpuMaterialExtension, clearcoatRoughnessFactor),
            LAYOUT_FIELD(GpuMaterialExtension, clearcoat),
            LAYOUT_FIELD(GpuMaterialExtension, clearcoatRoughness),
            LAYOUT_FIELD(GpuMaterialExtension, clearcoatNormal),
            LAYOUT_FIELD(GpuMaterialExtension, sheenColorFactor),
            LAYOUT_FIELD(GpuMaterialExtension, sheenRoughnessFactor),
            LAYOUT_FIELD(GpuMaterialExtension, sheenColor),
            LAYOUT_FIELD(GpuMaterialExtension, sheenRoughness),
            LAYOUT_FIELD(GpuMaterialExtension, transmissionFactor),
            LAYOUT_FIELD(GpuMaterialExtension, transmission),
            LAYOUT_FIELD(GpuMaterialExtension, thicknessFactor),
            LAYOUT_FIELD(GpuMaterialExtension, attenuationDistance),
            LAYOUT_FIELD(GpuMaterialExtension, attenuationColor),
            LAYOUT_FIELD(GpuMaterialExtension, thickness),
            LAYOUT_FIELD(GpuMaterialExtension, iridescenceFactor),
            LAYOUT_FIELD(GpuMaterialExtension, iridescenceIor),
            LAYOUT_FIELD(GpuMaterialExtension, iridescenceThicknessMin),
            LAYOUT_FIELD(GpuMaterialExtension, iridescenceThicknessMax),
            LAYOUT_FIELD(GpuMaterialExtension, iridescence),
            LAYOUT_FIELD(GpuMaterialExtension, iridescenceThickness),
            LAYOUT_FIELD(GpuMaterialExtension, anisotropyStrength),
            LAYOUT_FIELD(GpuMaterialExtension, anisotropyRotation),
            LAYOUT_FIELD(GpuMaterialExtension, anisotropy),
            LAYOUT_FIELD(GpuMaterialExtension, diffuseTransmissionFactor),
            LAYOUT_FIELD(GpuMaterialExtension, diffuseTransmissionColorFactor),
            LAYOUT_FIELD(GpuMaterialExtension, diffuseTransmission),
            LAYOUT_FIELD(GpuMaterialExtension, diffuseTransmissionColor),
        }},
        {"MeshPushData", sizeof(MeshPushData), {
            LAYOUT_FIELD(MeshPushData, frameRoot),
            LAYOUT_FIELD(MeshPushData, firstVisible),
            LAYOUT_FIELD(MeshPushData, meshletCount),
        }},
        {"MeshDgcSequence", sizeof(MeshDgcSequence), {
            LAYOUT_FIELD_AS(MeshDgcSequence, push, pushData),
            LAYOUT_FIELD_AS(MeshDgcSequence, groupCount, draw),
            // C++ pads through alignas(16)
            FieldLayout{"pad0", offsetof(MeshDgcSequence, draw) + sizeof(vk::DrawMeshTasksIndirectCommandEXT)},
        }},
        {"LightWorldPushData", sizeof(LightWorldPushData), {
            LAYOUT_FIELD(LightWorldPushData, frameRoot),
            LAYOUT_FIELD(LightWorldPushData, sunDirection),
            LAYOUT_FIELD(LightWorldPushData, sunIlluminance),
            LAYOUT_FIELD(LightWorldPushData, sunColor),
            LAYOUT_FIELD(LightWorldPushData, assetLightCount),
            LAYOUT_FIELD(LightWorldPushData, triangleLightTasks),
            LAYOUT_FIELD(LightWorldPushData, triangleLightTaskCount),
            LAYOUT_FIELD(LightWorldPushData, lightPdf),
            LAYOUT_FIELD(LightWorldPushData, lightPdfTexelCount),
            LAYOUT_FIELD(LightWorldPushData, pad0),
        }},
        {"LightPdfMipsPushData", sizeof(LightPdfMipsPushData), {
            LAYOUT_FIELD(LightPdfMipsPushData, srcImage),
            LAYOUT_FIELD(LightPdfMipsPushData, dstImage),
            LAYOUT_FIELD(LightPdfMipsPushData, dstSize),
        }},
        {"RtInstancesPushData", sizeof(RtInstancesPushData), {
            LAYOUT_FIELD(RtInstancesPushData, frameRoot),
            LAYOUT_FIELD(RtInstancesPushData, buildRows),
            LAYOUT_FIELD(RtInstancesPushData, writeInfos),
            LAYOUT_FIELD(RtInstancesPushData, operation),
            LAYOUT_FIELD(RtInstancesPushData, ptlasState),
            LAYOUT_FIELD(RtInstancesPushData, count),
            LAYOUT_FIELD(RtInstancesPushData, flags),
        }},
        {"CullPushData", sizeof(CullPushData), {
            LAYOUT_FIELD(CullPushData, frameRoot),
            LAYOUT_FIELD(CullPushData, cullView),
        }},
        {"HiZViewPushData", sizeof(HiZViewPushData), {
            LAYOUT_FIELD(HiZViewPushData, pyramid),
            LAYOUT_FIELD(HiZViewPushData, mip),
            LAYOUT_FIELD(HiZViewPushData, nearZ),
            LAYOUT_FIELD(HiZViewPushData, farZ),
            LAYOUT_FIELD(HiZViewPushData, screenSize),
        }},
        {"HiZPushData", sizeof(HiZPushData), {
            LAYOUT_FIELD(HiZPushData, srcImage),
            LAYOUT_FIELD(HiZPushData, dstImages),
            LAYOUT_FIELD(HiZPushData, srcMip),
            LAYOUT_FIELD(HiZPushData, isFromDepth),
            LAYOUT_FIELD(HiZPushData, levelCount),
            LAYOUT_FIELD(HiZPushData, pad0),
            LAYOUT_FIELD(HiZPushData, srcSize),
            LAYOUT_FIELD(HiZPushData, dstSize),
        }},
        {"FramePushData", sizeof(FramePushData), {
            LAYOUT_FIELD(FramePushData, frameRoot),
        }},
        {"ResolvePushData", sizeof(ResolvePushData), {
            LAYOUT_FIELD(ResolvePushData, frameRoot),
            LAYOUT_FIELD(ResolvePushData, tileList),
        }},
        {"ResolveDgcSequence", sizeof(ResolveDgcSequence), {
            LAYOUT_FIELD(ResolveDgcSequence, executionSetIndex),
            LAYOUT_FIELD(ResolveDgcSequence, pad0),
            LAYOUT_FIELD_AS(ResolveDgcSequence, push, pushData),
            LAYOUT_FIELD_AS(ResolveDgcSequence, groupCount, dispatch),
            LAYOUT_FIELD(ResolveDgcSequence, pad1),
        }},
        {"GpuMaterialBins", sizeof(GpuMaterialBins), {
            LAYOUT_FIELD(GpuMaterialBins, tileCount),
            LAYOUT_FIELD(GpuMaterialBins, sequenceCount),
            LAYOUT_FIELD(GpuMaterialBins, sequences),
        }},
        {"ExposurePushData", sizeof(ExposurePushData), {
            LAYOUT_FIELD(ExposurePushData, frameRoot),
            LAYOUT_FIELD(ExposurePushData, histogram),
            LAYOUT_FIELD(ExposurePushData, readback),
            LAYOUT_FIELD(ExposurePushData, deltaSeconds),
            LAYOUT_FIELD(ExposurePushData, compensationEv),
            LAYOUT_FIELD(ExposurePushData, manualEv),
            LAYOUT_FIELD(ExposurePushData, minEv),
            LAYOUT_FIELD(ExposurePushData, maxEv),
            LAYOUT_FIELD(ExposurePushData, adaptationSpeedUp),
            LAYOUT_FIELD(ExposurePushData, adaptationSpeedDown),
            LAYOUT_FIELD(ExposurePushData, flags),
        }},
        {"MotionCheckPushData", sizeof(MotionCheckPushData), {
            LAYOUT_FIELD(MotionCheckPushData, frameRoot),
            LAYOUT_FIELD(MotionCheckPushData, stats),
        }},
        {"GpuMotionCheckStats", sizeof(GpuMotionCheckStats), {
            LAYOUT_FIELD(GpuMotionCheckStats, pixels),
            LAYOUT_FIELD(GpuMotionCheckStats, emptyPixels),
            LAYOUT_FIELD(GpuMotionCheckStats, movingPixels),
            LAYOUT_FIELD(GpuMotionCheckStats, cameraMismatches),
            LAYOUT_FIELD(GpuMotionCheckStats, reprojectedErrors),
            LAYOUT_FIELD(GpuMotionCheckStats, stillErrors),
            LAYOUT_FIELD(GpuMotionCheckStats, offscreenPixels),
            LAYOUT_FIELD(GpuMotionCheckStats, frameFlags),
            LAYOUT_FIELD(GpuMotionCheckStats, maxMotionBits),
            LAYOUT_FIELD(GpuMotionCheckStats, maxCameraErrorBits),
        }},
        {"RayCheckPushData", sizeof(RayCheckPushData), {
            LAYOUT_FIELD(RayCheckPushData, frameRoot),
            LAYOUT_FIELD(RayCheckPushData, stats),
        }},
        {"SerCheckPushData", sizeof(SerCheckPushData), {
            LAYOUT_FIELD(SerCheckPushData, frameRoot),
            LAYOUT_FIELD(SerCheckPushData, stats),
        }},
        {"GpuSerCheckStats", sizeof(GpuSerCheckStats), {
            LAYOUT_FIELD(GpuSerCheckStats, rays),
            LAYOUT_FIELD(GpuSerCheckStats, hits),
            LAYOUT_FIELD(GpuSerCheckStats, hitMismatches),
            LAYOUT_FIELD(GpuSerCheckStats, idMismatches),
            LAYOUT_FIELD(GpuSerCheckStats, dataMismatches),
            LAYOUT_FIELD(GpuSerCheckStats, maxBarycentricErrorBits),
            LAYOUT_FIELD(GpuSerCheckStats, maxDistanceErrorBits),
        }},
        {"GpuRayCheckStats", sizeof(GpuRayCheckStats), {
            LAYOUT_FIELD(GpuRayCheckStats, coveredPixels),
            LAYOUT_FIELD(GpuRayCheckStats, interiorPixels),
            LAYOUT_FIELD(GpuRayCheckStats, misses),
            LAYOUT_FIELD(GpuRayCheckStats, idMismatches),
            LAYOUT_FIELD(GpuRayCheckStats, backFaceHits),
            LAYOUT_FIELD(GpuRayCheckStats, edgeMismatches),
            LAYOUT_FIELD(GpuRayCheckStats, emptyHits),
            LAYOUT_FIELD(GpuRayCheckStats, selfHits),
            LAYOUT_FIELD(GpuRayCheckStats, mipOutliers),
            LAYOUT_FIELD(GpuRayCheckStats, depthTies),
            LAYOUT_FIELD(GpuRayCheckStats, maxNormalErrorBits),
            LAYOUT_FIELD(GpuRayCheckStats, maxGeoNormalErrorBits),
            LAYOUT_FIELD(GpuRayCheckStats, maxPositionErrorBits),
            LAYOUT_FIELD(GpuRayCheckStats, maxMipErrorBits),
            LAYOUT_FIELD(GpuRayCheckStats, maxMaterialNormalErrorBits),
            LAYOUT_FIELD(GpuRayCheckStats, maxHitNormalErrorBits),
            LAYOUT_FIELD(GpuRayCheckStats, maxHitMaterialNormalErrorBits),
            LAYOUT_FIELD(GpuRayCheckStats, alphaEdges),
        }},
        {"GpuRayDebugCounters", sizeof(GpuRayDebugCounters), {
            LAYOUT_FIELD(GpuRayDebugCounters, candidates),
        }},
        {"PresentPushData", sizeof(PresentPushData), {
            LAYOUT_FIELD(PresentPushData, frameRoot),
            LAYOUT_FIELD(PresentPushData, displaySize),
        }},
        {"GpuGuideSet", sizeof(GpuGuideSet), {
            LAYOUT_FIELD(GpuGuideSet, storage),
            LAYOUT_FIELD(GpuGuideSet, sampled),
        }},
        {"GpuTargets", sizeof(GpuTargets), {
            LAYOUT_FIELD(GpuTargets, visBuffer),
            LAYOUT_FIELD(GpuTargets, depth),
            LAYOUT_FIELD(GpuTargets, displayStorage),
            LAYOUT_FIELD(GpuTargets, displaySampled),
            LAYOUT_FIELD(GpuTargets, linearSampler),
            LAYOUT_FIELD(GpuTargets, pad0),
            LAYOUT_FIELD(GpuTargets, renderSize),
            LAYOUT_FIELD(GpuTargets, current),
            LAYOUT_FIELD(GpuTargets, previous),
            LAYOUT_FIELD(GpuTargets, directRadianceStorage),
            LAYOUT_FIELD(GpuTargets, directRadianceSampled),
            LAYOUT_FIELD(GpuTargets, indirectRadianceStorage),
            LAYOUT_FIELD(GpuTargets, indirectRadianceSampled),
            LAYOUT_FIELD(GpuTargets, noisyColorStorage),
            LAYOUT_FIELD(GpuTargets, noisyColorSampled),
            LAYOUT_FIELD(GpuTargets, presentSampled),
            LAYOUT_FIELD(GpuTargets, upscaledSampled),
            LAYOUT_FIELD(GpuTargets, postStorage),
            LAYOUT_FIELD(GpuTargets, exposureStorage),
            LAYOUT_FIELD(GpuTargets, displaySize),
            LAYOUT_FIELD(GpuTargets, binIdsStorage),
            LAYOUT_FIELD(GpuTargets, binIdsSampled),
            LAYOUT_FIELD(GpuTargets, giStagnancyStorage),
            LAYOUT_FIELD(GpuTargets, giStagnancyPrevious),
            LAYOUT_FIELD(GpuTargets, giSampleIdsStorage),
            LAYOUT_FIELD(GpuTargets, giDuplicationStorage),
            LAYOUT_FIELD(GpuTargets, glassDiffuseAlbedoStorage),
            LAYOUT_FIELD(GpuTargets, glassSpecAlbedoStorage),
        }},
        {"GpuExposureReadback", sizeof(GpuExposureReadback), {
            LAYOUT_FIELD(GpuExposureReadback, exposure),
            LAYOUT_FIELD(GpuExposureReadback, targetExposure),
            LAYOUT_FIELD(GpuExposureReadback, averageLuminance),
            LAYOUT_FIELD(GpuExposureReadback, pixels),
        }},
        {"GpuRayInstance", sizeof(GpuRayInstance), {
            LAYOUT_FIELD(GpuRayInstance, nodeInstanceIndex),
            LAYOUT_FIELD(GpuRayInstance, primitiveRow),
        }},
        {"GpuTriangleLightTask", sizeof(GpuTriangleLightTask), {
            LAYOUT_FIELD(GpuTriangleLightTask, firstLight),
            LAYOUT_FIELD(GpuTriangleLightTask, triangleCount),
            LAYOUT_FIELD(GpuTriangleLightTask, rayInstance),
        }},
        {"GpuRayInstanceBuild", sizeof(GpuRayInstanceBuild), {
            LAYOUT_FIELD(GpuRayInstanceBuild, blas),
            LAYOUT_FIELD(GpuRayInstanceBuild, partition),
            LAYOUT_FIELD(GpuRayInstanceBuild, flags),
        }},
        {"GpuFrameRoot", sizeof(GpuFrameRoot), {
            LAYOUT_FIELD(GpuFrameRoot, camera),
            LAYOUT_FIELD(GpuFrameRoot, instances),
            LAYOUT_FIELD(GpuFrameRoot, models),
            LAYOUT_FIELD(GpuFrameRoot, primitives),
            LAYOUT_FIELD(GpuFrameRoot, materials),
            LAYOUT_FIELD(GpuFrameRoot, pbrExtensions),
            LAYOUT_FIELD(GpuFrameRoot, lights),
            LAYOUT_FIELD(GpuFrameRoot, tlas),
            LAYOUT_FIELD(GpuFrameRoot, nodeInstances),
            LAYOUT_FIELD(GpuFrameRoot, drawRecords),
            LAYOUT_FIELD(GpuFrameRoot, visibleMeshlets),
            LAYOUT_FIELD(GpuFrameRoot, lightCount),
            LAYOUT_FIELD(GpuFrameRoot, instanceCount),
            LAYOUT_FIELD(GpuFrameRoot, primitiveCount),
            LAYOUT_FIELD(GpuFrameRoot, nodeInstanceCount),
            LAYOUT_FIELD(GpuFrameRoot, lateVisibleBase),
            LAYOUT_FIELD(GpuFrameRoot, debugView),
            LAYOUT_FIELD(GpuFrameRoot, cullCounters),
            LAYOUT_FIELD(GpuFrameRoot, triangleCull),
            LAYOUT_FIELD(GpuFrameRoot, subpixelSnap),
            LAYOUT_FIELD(GpuFrameRoot, targets),
            LAYOUT_FIELD(GpuFrameRoot, frameFlags),
            LAYOUT_FIELD(GpuFrameRoot, localLightCount),
            LAYOUT_FIELD(GpuFrameRoot, rayInstances),
            LAYOUT_FIELD(GpuFrameRoot, rtxdiConstants),
            LAYOUT_FIELD(GpuFrameRoot, cacheConstants),
            LAYOUT_FIELD(GpuFrameRoot, frameIndex),
            LAYOUT_FIELD(GpuFrameRoot, exposure),
            LAYOUT_FIELD(GpuFrameRoot, worldLights),
            LAYOUT_FIELD(GpuFrameRoot, prevWorldLights),
            LAYOUT_FIELD(GpuFrameRoot, rayDebug),
            LAYOUT_FIELD(GpuFrameRoot, materialBins),
            LAYOUT_FIELD(GpuFrameRoot, rayInstanceLights),
            LAYOUT_FIELD(GpuFrameRoot, firstTriangleLight),
            LAYOUT_FIELD(GpuFrameRoot, triangleLightCount),
            LAYOUT_FIELD(GpuFrameRoot, glassFlags),
            LAYOUT_FIELD(GpuFrameRoot, glassMaxLayers),
            LAYOUT_FIELD(GpuFrameRoot, glassCounters),
        }},
    };

    // textual mirror next to RTXDI's parameter structs, reflected through its own source module
    const StructLayout kRtxdiConstants = {"GpuRtxdiConstants", sizeof(GpuRtxdiConstants), {
        LAYOUT_FIELD(GpuRtxdiConstants, runtime),
        LAYOUT_FIELD(GpuRtxdiConstants, lightBuffer),
        LAYOUT_FIELD(GpuRtxdiConstants, reservoirBuffer),
        LAYOUT_FIELD(GpuRtxdiConstants, bufferIndices),
        LAYOUT_FIELD(GpuRtxdiConstants, initialSampling),
        LAYOUT_FIELD(GpuRtxdiConstants, temporalResampling),
        LAYOUT_FIELD(GpuRtxdiConstants, boilingFilter),
        LAYOUT_FIELD(GpuRtxdiConstants, spatialResampling),
        LAYOUT_FIELD(GpuRtxdiConstants, shading),
        LAYOUT_FIELD(GpuRtxdiConstants, reservoirs),
        LAYOUT_FIELD(GpuRtxdiConstants, neighborOffsets),
        LAYOUT_FIELD(GpuRtxdiConstants, resamplingMode),
        LAYOUT_FIELD(GpuRtxdiConstants, reservoirCount),
        LAYOUT_FIELD(GpuRtxdiConstants, giReservoirBuffer),
        LAYOUT_FIELD(GpuRtxdiConstants, giBufferIndices),
        LAYOUT_FIELD(GpuRtxdiConstants, giReservoirs),
        LAYOUT_FIELD(GpuRtxdiConstants, giReservoirCount),
        LAYOUT_FIELD(GpuRtxdiConstants, giResamplingMode),
        LAYOUT_FIELD(GpuRtxdiConstants, giTemporalResampling),
        LAYOUT_FIELD(GpuRtxdiConstants, giBoilingFilter),
        LAYOUT_FIELD(GpuRtxdiConstants, giSpatialResampling),
        LAYOUT_FIELD(GpuRtxdiConstants, giFinalShading),
        LAYOUT_FIELD(GpuRtxdiConstants, giReferenceBounces),
        LAYOUT_FIELD(GpuRtxdiConstants, giFlags),
        LAYOUT_FIELD(GpuRtxdiConstants, localLightRisBuffer),
        LAYOUT_FIELD(GpuRtxdiConstants, environmentLightRisBuffer),
        LAYOUT_FIELD(GpuRtxdiConstants, regir),
        LAYOUT_FIELD(GpuRtxdiConstants, risBuffer),
        LAYOUT_FIELD(GpuRtxdiConstants, risLightData),
        LAYOUT_FIELD(GpuRtxdiConstants, risEntryCount),
        LAYOUT_FIELD(GpuRtxdiConstants, lightPdf),
        LAYOUT_FIELD(GpuRtxdiConstants, lightPdfSize),
        LAYOUT_FIELD(GpuRtxdiConstants, giDecorrelation),
        LAYOUT_FIELD(GpuRtxdiConstants, giHistoryReductionStrength),
        LAYOUT_FIELD(GpuRtxdiConstants, pad0),
        LAYOUT_FIELD(GpuRtxdiConstants, diNeighborOffsets),
    }};

    // textual mirror next to the NRC SDK's NrcConstants
    const StructLayout kNrcConstants = {"GpuNrcConstants", sizeof(GpuNrcConstants), {
        LAYOUT_FIELD(GpuNrcConstants, nrc),
        LAYOUT_FIELD(GpuNrcConstants, queryPathInfo),
        LAYOUT_FIELD(GpuNrcConstants, trainingPathInfo),
        LAYOUT_FIELD(GpuNrcConstants, trainingPathVertices),
        LAYOUT_FIELD(GpuNrcConstants, queryRadianceParams),
        LAYOUT_FIELD(GpuNrcConstants, queryRadiance),
        LAYOUT_FIELD(GpuNrcConstants, counters),
        LAYOUT_FIELD(GpuNrcConstants, queryPathInfoCount),
        LAYOUT_FIELD(GpuNrcConstants, trainingPathInfoCount),
        LAYOUT_FIELD(GpuNrcConstants, trainingPathVertexCount),
        LAYOUT_FIELD(GpuNrcConstants, queryRadianceCount),
        LAYOUT_FIELD(GpuNrcConstants, flags),
        LAYOUT_FIELD(GpuNrcConstants, trainingLaunch),
    }};
    // clang-format on

#undef LAYOUT_FIELD_AS
#undef LAYOUT_FIELD

    struct Reflected
    {
        std::string name;
        size_t size;
        std::vector<std::pair<std::string, size_t>> fields;
    };

    struct ImageFormatConstant
    {
        std::string_view name;
        SlangImageFormat format;
    };

    // core/binding.slang storage-image formats
    constexpr std::array kImageFormats = {
        ImageFormatConstant{"kImageFormatRgba16f", SLANG_IMAGE_FORMAT_rgba16f},
        ImageFormatConstant{"kImageFormatRg16f", SLANG_IMAGE_FORMAT_rg16f},
        ImageFormatConstant{"kImageFormatR11g11b10f", SLANG_IMAGE_FORMAT_r11f_g11f_b10f},
        ImageFormatConstant{"kImageFormatR32f", SLANG_IMAGE_FORMAT_r32f},
        ImageFormatConstant{"kImageFormatRgba8", SLANG_IMAGE_FORMAT_rgba8},
        ImageFormatConstant{"kImageFormatRgba32ui", SLANG_IMAGE_FORMAT_rgba32ui},
        ImageFormatConstant{"kImageFormatR32ui", SLANG_IMAGE_FORMAT_r32ui},
        ImageFormatConstant{"kImageFormatR8ui", SLANG_IMAGE_FORMAT_r8ui},
    };

    // the engine's slangc flags that change layout: -target spirv, -fvk-use-scalar-layout, column-major
    struct SlangContext
    {
        Slang::ComPtr<slang::IGlobalSession> globalSession;
        Slang::ComPtr<slang::ISession> session;

        bool create()
        {
            if (!check(SLANG_SUCCEEDED(slang::createGlobalSession(globalSession.writeRef())),
                       "slang global session created")) {
                return false;
            }
            slang::TargetDesc target{};
            target.format = SLANG_SPIRV;
            target.profile = globalSession->findProfile("sm_6_9");
            target.forceGLSLScalarBufferLayout = true;

            const char* searchPaths[] = {ENGINE_SHADER_SOURCE_DIR, ENGINE_RTXDI_INCLUDE_DIR, ENGINE_NRC_INCLUDE_DIR};
            slang::SessionDesc sessionDesc{};
            sessionDesc.targets = &target;
            sessionDesc.targetCount = 1;
            sessionDesc.searchPaths = searchPaths;
            sessionDesc.searchPathCount = static_cast<SlangInt>(std::size(searchPaths));
            sessionDesc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;
            return check(SLANG_SUCCEEDED(globalSession->createSession(sessionDesc, session.writeRef())),
                         "slang session created");
        }

        slang::IModule* load(const char* moduleName)
        {
            Slang::ComPtr<slang::IBlob> diagnostics;
            slang::IModule* module = session->loadModule(moduleName, diagnostics.writeRef());
            if (diagnostics) {
                std::printf("%s\n", static_cast<const char*>(diagnostics->getBufferPointer()));
            }
            check(module != nullptr, std::format("{} loaded", moduleName).c_str());
            return module;
        }

        // a textual file (no module declaration) as its own module
        slang::IModule* loadText(const char* moduleName, const char* includePath)
        {
            const std::string source = std::format("#include \"{}\"\n", includePath);
            const std::string path = std::format("{}.slang", moduleName);
            Slang::ComPtr<slang::IBlob> diagnostics;
            slang::IModule* module =
                session->loadModuleFromSourceString(moduleName, path.c_str(), source.c_str(), diagnostics.writeRef());
            if (diagnostics) {
                std::printf("%s\n", static_cast<const char*>(diagnostics->getBufferPointer()));
            }
            check(module != nullptr, std::format("{} loaded", includePath).c_str());
            return module;
        }
    };

    std::vector<Reflected> reflectTypes(slang::IModule& module)
    {
        slang::ProgramLayout* programLayout = module.getLayout();
        slang::DeclReflection* root = module.getModuleReflection();
        std::vector<Reflected> types;
        for (unsigned int i = 0; i < root->getChildrenCount(); ++i) {
            slang::DeclReflection* decl = root->getChild(i);
            if (decl->getKind() != slang::DeclReflection::Kind::Struct) {
                continue;
            }
            // BDA data: storage-buffer rules, scalar through forceGLSLScalarBufferLayout
            slang::TypeLayoutReflection* layout =
                programLayout->getTypeLayout(decl->getType(), slang::LayoutRules::DefaultStructuredBuffer);
            Reflected type{.name = decl->getName(), .size = layout->getSize(), .fields = {}};
            for (unsigned int f = 0; f < layout->getFieldCount(); ++f) {
                slang::VariableLayoutReflection* field = layout->getFieldByIndex(f);
                type.fields.emplace_back(field->getName(), field->getOffset());
            }
            types.push_back(std::move(type));
        }
        return types;
    }

    // the format generic argument is a raw SlangImageFormat value; a wrong one types the image as another format
    void verifyImageFormats(slang::IModule& module)
    {
        std::printf("core/binding.slang image formats\n");
        slang::DeclReflection* root = module.getModuleReflection();
        for (const ImageFormatConstant& expected : kImageFormats) {
            std::optional<int32_t> value;
            for (unsigned int i = 0; i < root->getChildrenCount(); ++i) {
                slang::DeclReflection* decl = root->getChild(i);
                if (decl->getKind() != slang::DeclReflection::Kind::Variable || decl->getName() != expected.name) {
                    continue;
                }
                // C entry point: getDefaultValueBlob is a C++ export, MSVC-mangled, so MinGW cannot link it
                int64_t constant = 0;
                if (SLANG_SUCCEEDED(spReflectionVariable_GetDefaultValueInt(
                        reinterpret_cast<SlangReflectionVariable*>(decl->asVariable()), &constant))) {
                    value = static_cast<int32_t>(constant);
                }
            }
            check(value == static_cast<int32_t>(expected.format),
                  std::format("  {} = {}", expected.name, static_cast<int32_t>(expected.format)).c_str());
        }
    }

    void dump(std::span<const Reflected> types)
    {
        for (const Reflected& type : types) {
            std::printf("%s size %zu\n", type.name.c_str(), type.size);
            for (const auto& [name, offset] : type.fields) {
                std::printf("  %-28s %zu\n", name.c_str(), offset);
            }
        }
    }

    const StructLayout* findMirror(std::string_view name)
    {
        for (const StructLayout& mirror : kMirrors) {
            if (mirror.name == name) {
                return &mirror;
            }
        }
        return nullptr;
    }

    void verify(const Reflected& type, const StructLayout& mirror)
    {
        std::printf("%s\n", type.name.c_str());
        check(type.size == mirror.size, std::format("  size: slang {} c++ {}", type.size, mirror.size).c_str());
        check(type.fields.size() == mirror.fields.size(),
              std::format("  field count: slang {} c++ {}", type.fields.size(), mirror.fields.size()).c_str());
        for (const auto& [name, offset] : type.fields) {
            const auto field = std::ranges::find(mirror.fields, std::string_view(name), &FieldLayout::name);
            if (!check(field != mirror.fields.end(), std::format("  {} mirrored", name).c_str())) {
                continue;
            }
            check(field->offset == offset, std::format("  {}: slang {} c++ {}", name, offset, field->offset).c_str());
        }
    }
} // namespace

int main(int argc, char** argv)
{
    SlangContext slang;
    if (!slang.create()) {
        return test::finish();
    }
    slang::IModule* typesModule = slang.load("types");
    if (typesModule == nullptr) {
        return test::finish();
    }
    const std::vector<Reflected> types = reflectTypes(*typesModule);
    if (argc > 1 && std::string_view(argv[1]) == "--dump") {
        dump(types);
        return 0;
    }
    check(!types.empty(), "types.slang declares structs");
    for (const Reflected& type : types) {
        if (std::ranges::contains(kShaderOnly, std::string_view(type.name))) {
            continue;
        }
        const StructLayout* mirror = findMirror(type.name);
        if (check(mirror != nullptr, std::format("{} has a C++ mirror", type.name).c_str())) {
            verify(type, *mirror);
        }
    }
    if (slang::IModule* rtxdiModule = slang.loadText("rtxdi_constants_layout", "lighting/rtxdi_constants.slang")) {
        const std::vector<Reflected> rtxdiTypes = reflectTypes(*rtxdiModule);
        const auto constants = std::ranges::find(rtxdiTypes, kRtxdiConstants.name, &Reflected::name);
        if (check(constants != rtxdiTypes.end(), "rtxdi_constants.slang declares GpuRtxdiConstants")) {
            verify(*constants, kRtxdiConstants);
        }
    }
    if (slang::IModule* nrcModule = slang.loadText("nrc_constants_layout", "gi/nrc_constants.slang")) {
        const std::vector<Reflected> nrcTypes = reflectTypes(*nrcModule);
        const auto constants = std::ranges::find(nrcTypes, kNrcConstants.name, &Reflected::name);
        if (check(constants != nrcTypes.end(), "nrc_constants.slang declares GpuNrcConstants")) {
            verify(*constants, kNrcConstants);
        }
    }
    if (slang::IModule* bindingModule = slang.load("core/binding")) {
        verifyImageFormats(*bindingModule);
    }
    return test::finish();
}
