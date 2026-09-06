set(UTIL_SOURCES
        debug.cpp
        ../static_headers/logger.cpp
        maths.cpp
        vk_tracy.cpp
        vk_shaders.cpp
        vk_utils.cpp
)

add_library(engine_util SHARED ${UTIL_SOURCES})
target_compile_definitions(engine_util PRIVATE ENGINE_LOG_DLL=\"util\")
target_link_libraries(engine_util PUBLIC engine_build_config ThirdParty::all ThirdPartyImpl)
target_precompile_headers(engine_util PRIVATE ${PCH_HEADER})
