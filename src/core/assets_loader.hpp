#pragma once
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

    void processVertexData(const tinyobj::attrib_t& attrib, const std::vector<tinyobj::shape_t>& shapes);
    void loadMaterials(const std::string& path, const std::vector<tinyobj::material_t>& materials);

    ObjectStorage& objectStorage;
    TextureManager& textureManager;
    GeometryStore& geometryStore;
    MaterialStore& materialStore;
    LightStore& lightStore;

private:
    [[nodiscard]] std::optional<EntityId> loadGltfModel(const std::string& modelPath, glm::vec3 xyz);
    [[nodiscard]] std::optional<EntityId> loadObjModel(const std::string& modelPath, glm::vec3 xyz);
};
