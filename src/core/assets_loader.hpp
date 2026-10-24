#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "geometry_store.hpp"
#include "light_store.hpp"
#include "material_store.hpp"
#include "object_storage.hpp"
#include "texture_manager.hpp"
#include "types.hpp"


class AssetsLoader
{
public:
    explicit AssetsLoader(ObjectStorage& objectStorage, TextureManager& textureManager, GeometryStore& geometryStore,
                          MaterialStore& materialStore, LightStore& lightStore);
    ~AssetsLoader() = default;

    // Load a model into CPU mesh vectors and create a SoA entity in objectStorage.
    // nullopt on failure; flush the scratch to the GPU before the next load (one model per flush).
    [[nodiscard]] std::optional<EntityId> loadModel(std::string modelPath, glm::vec3 xyz);
    // model file a folder pack or file path loads; also the model cache key. nullopt when the folder has none
    [[nodiscard]] static std::optional<std::filesystem::path> resolveModelPath(const std::filesystem::path& modelPath);

    ObjectStorage& objectStorage;
    TextureManager& textureManager;
    GeometryStore& geometryStore;
    MaterialStore& materialStore;
    LightStore& lightStore;

private:
    [[nodiscard]] std::optional<EntityId> loadGltfModel(const std::string& modelPath, glm::vec3 xyz);
    [[nodiscard]] std::optional<EntityId> loadObjModel(const std::string& modelPath, glm::vec3 xyz);
};
