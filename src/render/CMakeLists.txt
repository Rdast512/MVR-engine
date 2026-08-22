set(RENDER_SOURCES
    vk_dgc.cpp
    vk_pipeline.cpp
    vk_renderer.cpp
    vk_pbr.cpp
)

add_library(engine_render SHARED ${RENDER_SOURCES})
target_link_libraries(engine_render PUBLIC engine_build_config engine_core engine_util engine_scene ThirdParty::all)
target_precompile_headers(engine_render PRIVATE ${PCH_HEADER})
