set(UTIL_SOURCES
        debug.cpp
        ../static_headers/logger.cpp
        vk_tracy.cpp
        vk_utils.cpp
)

add_library(engine_mi_new_delete OBJECT mi_new_delete.cpp)
set_target_properties(engine_mi_new_delete PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_source_files_properties(mi_new_delete.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
# also linked into uninstrumented fmt/spdlog; profile hooks would need the runtime
target_compile_options(engine_mi_new_delete PRIVATE -fno-profile-instr-generate)
# native code: lld binds wrapped __imp_ references (dllimport callers such as NGX) to the __imp___wrap_* slots
# before LTO runs; bitcode slots leave those calls unresolved
target_compile_options(engine_mi_new_delete PRIVATE -fno-lto)
target_link_libraries(engine_mi_new_delete PRIVATE ThirdParty::mimalloc)

add_library(engine_util SHARED ${UTIL_SOURCES})
target_compile_definitions(engine_util PRIVATE ENGINE_LOG_DLL=\"util\")
# the only module holding the single-header implementations (VMA, stb, tinygltf); auto-export serves the others
target_link_libraries(engine_util PUBLIC engine_build_config ThirdParty::all
        PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,ThirdPartyImpl>")
target_precompile_headers(engine_util PRIVATE ${PCH_HEADER})
