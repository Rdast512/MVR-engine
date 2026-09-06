set(POSTPROCESSING_SOURCES
        vk_postprocessing.cpp
)

add_library(engine_postprocessing SHARED ${POSTPROCESSING_SOURCES})
target_compile_definitions(engine_postprocessing PRIVATE ENGINE_LOG_DLL=\"post\")
target_link_libraries(engine_postprocessing PUBLIC engine_build_config engine_core engine_util ThirdParty::all)
target_precompile_headers(engine_postprocessing PRIVATE ${PCH_HEADER})
