set(RENDER_SOURCES
    vk_dgc.cpp
    vk_dlss.cpp
        vk_post.cpp
    vk_gpu_timer.cpp
    vk_hiz.cpp
    vk_meshlet_culling.cpp
    vk_render_targets.cpp
    vk_renderer.cpp
    vk_rt_scene.cpp
    vk_rt_pipeline.cpp
    vk_rtxdi.cpp
    vk_world_lights.cpp
    vk_shader_objects.cpp
)

add_library(engine_render SHARED ${RENDER_SOURCES})
target_compile_definitions(engine_render PRIVATE ENGINE_LOG_DLL=\"render\")
target_link_libraries(engine_render PUBLIC engine_build_config engine_core engine_util engine_scene ThirdParty::all)
target_precompile_headers(engine_render PRIVATE ${PCH_HEADER})

if (WIN32)
    target_sources(engine_render PRIVATE nrc_library.cpp)
    target_link_libraries(engine_render PUBLIC ThirdParty::nrc)
    # NGX stays inside engine_render: its static lib keeps process-wide state
    target_link_libraries(engine_render PRIVATE ThirdParty::ngx)
    add_custom_command(TARGET engine_render POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ENGINE_NRC_RUNTIME_DLLS} $<TARGET_FILE_DIR:engine_render>
        COMMENT "Copying NRC runtime DLLs...")
    # NGX loads the DLSS feature dlls from the application folder
    add_custom_command(TARGET engine_render POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ENGINE_DLSS_RUNTIME_DLLS} $<TARGET_FILE_DIR:engine_render>
        COMMENT "Copying DLSS feature DLLs...")
endif ()
