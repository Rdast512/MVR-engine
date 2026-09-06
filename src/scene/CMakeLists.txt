set(SCENE_SOURCES
        vk_camera.cpp
        vk_mesh.cpp
        vk_scene.cpp
)

add_library(engine_scene SHARED ${SCENE_SOURCES})
target_compile_definitions(engine_scene PRIVATE ENGINE_LOG_DLL=\"scene\")
target_link_libraries(engine_scene PUBLIC engine_build_config engine_core engine_util ThirdParty::all)
target_precompile_headers(engine_scene PRIVATE ${PCH_HEADER})
