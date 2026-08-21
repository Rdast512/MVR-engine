set(CORE_SOURCES
    assets_loader.cpp
    texture_manager.cpp
    vk_allocator.cpp
    vk_descriptors.cpp
    vk_device.cpp
    vk_resource_manager.cpp
    vk_swapchain.cpp
    object_storage.cpp
)

add_library(engine_core SHARED ${CORE_SOURCES})
target_link_libraries(engine_core PUBLIC engine_build_config engine_util ThirdParty::all ThirdPartyImpl)
target_precompile_headers(engine_core PRIVATE ${PCH_HEADER})