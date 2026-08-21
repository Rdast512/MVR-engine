# Root CMake for engine source modules

set(PCH_HEADER ${CMAKE_SOURCE_DIR}/src/pch.h)

# [COVERAGE] Directory-scoped flags — apply to every engine_* shared lib AND
# the engine executable defined below. Off unless ENGINE_ENABLE_COVERAGE=ON.
#
# WHY all targets (not just the exe):
#   -fprofile-instr-generate at *compile* time instruments the code and emits
#   counter arrays. But without the SAME flag at *link* time, the linker never
#   emits the CRT initializer (__llvm_profile_init / .CRT$XCOV entry) that
#   registers those arrays with the profile runtime at DLL load time.
#   Result without it: engine_core.dll counters exist but are never seen by
#   the runtime → absent from the .profraw → absent from the coverage report.
#
# WHY not linking the runtime into each DLL:
#   -fprofile-instr-generate on an *executable* link automatically pulls in
#   libclang_rt.profile (static or shared depending on the toolchain). On a
#   *shared library* link, Clang emits only the registration hooks and skips
#   the runtime library itself — so each DLL registers with whichever runtime
#   is loaded (the exe’s), instead of bringing its own conflicting copy.
#
# LLVM_PROFILE_FILE=%m.profraw (set in the CLion run config) is recommended.
# %m expands to a per-module magic number, creating one .profraw per DLL + exe
# so each module flushes independently. CLion then merges them before calling
# llvm-cov export.
if (ENGINE_ENABLE_COVERAGE AND WIN32)
    add_compile_options("$<$<CONFIG:Debug>:-fprofile-instr-generate>")
    add_link_options("$<$<CONFIG:Debug>:-fprofile-instr-generate>")
endif ()

add_library(engine_build_config INTERFACE)
target_include_directories(engine_build_config INTERFACE ${CMAKE_SOURCE_DIR}/src)
target_include_directories(engine_build_config INTERFACE ${CMAKE_SOURCE_DIR}/src/static_headers)
target_compile_definitions(engine_build_config INTERFACE
    $<$<NOT:$<CONFIG:Release>>:TRACY_ENABLE>
    ENGINE_SHADER_DIR="${ENGINE_SHADER_DIR_PATH}"
    ENGINE_MODELS_DIR="${ENGINE_MODELS_DIR_PATH}"
    ENGINE_TEXTURES_DIR="${ENGINE_TEXTURES_DIR_PATH}"
)
target_compile_options(engine_build_config INTERFACE
    $<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang,GNU>:-Wall>
    $<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang,GNU>:-Wextra>
    $<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang,GNU>:-Wpedantic>
)

add_subdirectory(util)
add_subdirectory(core)
add_subdirectory(render)
add_subdirectory(scene)
add_subdirectory(postprocessing)

add_library(engine_runtime SHARED core/vk_engine.cpp core/vk_engine.hpp)
target_link_libraries(engine_runtime
    PUBLIC
        engine_build_config
        engine_core
        engine_render
        engine_util
        engine_postprocessing
        engine_scene
        ThirdParty::all
)

target_link_libraries(engine_runtime PRIVATE
        ${CMAKE_DL_LIBS}
)

target_precompile_headers(engine_runtime PRIVATE ${PCH_HEADER})

add_executable(engine main.cpp)
target_link_libraries(engine PRIVATE engine_runtime)
target_precompile_headers(engine PRIVATE ${PCH_HEADER})
add_dependencies(engine Shaders)

if(WIN32)
    # Copy third-party DLLs needed at runtime next to the executable
    set(SHARED_LIBS_TO_COPY
        SDL3::SDL3
        ktx
        fmt::fmt
        spdlog::spdlog
    )

    if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
        list(APPEND SHARED_LIBS_TO_COPY Tracy::TracyClient)
    endif()

    foreach(_LIB ${SHARED_LIBS_TO_COPY})
        if(TARGET ${_LIB})
            get_target_property(_type ${_LIB} TYPE)
            if("${_type}" STREQUAL "SHARED_LIBRARY" OR "${_type}" STREQUAL "MODULE_LIBRARY")
                add_custom_command(TARGET engine POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        $<TARGET_FILE:${_LIB}>
                        $<TARGET_FILE_DIR:engine>
                    COMMENT "Copying DLL for ${_LIB}...")
            endif()
        endif()
    endforeach()
    # engine_* DLLs already land in CMAKE_RUNTIME_OUTPUT_DIRECTORY (== binary dir)
else()
    # On Linux copy third-party shared libs next to the executable
    set(SHARED_LIBS_TO_COPY
        SDL3::SDL3
        ktx
        fmt::fmt
        spdlog::spdlog
        glm::glm
    )

    if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
        list(APPEND SHARED_LIBS_TO_COPY Tracy::TracyClient)
    endif()

    foreach(_LIB ${SHARED_LIBS_TO_COPY})
        if(TARGET ${_LIB})
            get_target_property(_type ${_LIB} TYPE)
            if("${_type}" STREQUAL "SHARED_LIBRARY" OR "${_type}" STREQUAL "MODULE_LIBRARY")
                add_custom_command(TARGET engine POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        $<TARGET_FILE:${_LIB}>
                        $<TARGET_FILE_DIR:engine>
                    COMMAND ${CMAKE_COMMAND} -E create_symlink
                        $<TARGET_FILE:${_LIB}>
                        $<TARGET_FILE_DIR:engine>/$<TARGET_SONAME_FILE_NAME:${_LIB}>
                    COMMAND ${CMAKE_COMMAND} -E create_symlink
                        $<TARGET_FILE:${_LIB}>
                        $<TARGET_FILE_DIR:engine>/$<TARGET_LINKER_FILE_NAME:${_LIB}>
                    COMMENT "Copying shared lib (and SONAME links) for ${_LIB}...")
            endif()
        endif()
    endforeach()

    foreach (_t engine engine_runtime engine_core engine_render engine_util engine_scene engine_postprocessing)
        if (TARGET ${_t})
            set_target_properties(${_t} PROPERTIES
                BUILD_WITH_INSTALL_RPATH TRUE
                INSTALL_RPATH "$ORIGIN"
                BUILD_RPATH "$ORIGIN")
        endif ()
    endforeach ()
endif()

if (ENGINE_UNITY_BUILD)
    foreach (_t engine_util engine_core engine_render engine_scene engine_runtime)
        if (TARGET ${_t})
            set_target_properties(${_t} PROPERTIES UNITY_BUILD ON)
        endif ()
    endforeach ()
endif ()
