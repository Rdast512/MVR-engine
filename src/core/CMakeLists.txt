set(CORE_SOURCES
    assets_loader.cpp
    geometry_store.cpp
    texture_manager.cpp
    vk_descriptors.cpp
    vk_device.cpp
    vk_asset_buffers.cpp
    vk_frame_data.cpp
    vk_frame_sync.cpp
    vk_mega_geometry.cpp
    vk_swapchain.cpp
    object_storage.cpp
)

find_package(Threads REQUIRED)

add_library(engine_core SHARED ${CORE_SOURCES})
target_compile_definitions(engine_core PRIVATE ENGINE_LOG_DLL=\"core\")
target_link_libraries(engine_core PUBLIC engine_build_config engine_util ThirdParty::all Threads::Threads)
target_precompile_headers(engine_core PRIVATE ${PCH_HEADER})