add_library(engine_test_support STATIC test_support.cpp)
target_include_directories(engine_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Vulkan::Vulkan is imported in ThirdParty scope only; the cppm wrapper is global
target_link_libraries(engine_test_support PUBLIC Vulkan::cppm)

# --- SHaRC: shader-only library, exercised as update -> resolve -> query on the GPU ---
set(_sharcShader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/sharc_roundtrip.slang")
set(_sharcShaderDir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
set(_sharcSpirv)
# SHARC_UPDATE / SHARC_QUERY are compile-time modes, so each pass is its own module
foreach (_pass "updateMain;-DSHARC_UPDATE=1" "resolveMain;" "queryMain;-DSHARC_QUERY=1")
    list(GET _pass 0 _entry)
    list(SUBLIST _pass 1 -1 _defines)
    set(_spv "${_sharcShaderDir}/${_entry}.spv")
    add_custom_command(
        OUTPUT ${_spv}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_sharcShaderDir}
        COMMAND ${SLANGC_EXECUTABLE} ${_sharcShader} ${ENGINE_SLANG_FLAGS} ${_defines} -entry ${_entry} -o ${_spv}
        DEPENDS ${_sharcShader} ${ENGINE_SLANG_THIRDPARTY_HEADERS}
        COMMENT "Compiling SHaRC test shader ${_entry}"
        VERBATIM
    )
    list(APPEND _sharcSpirv ${_spv})
endforeach ()

add_executable(sharc_test sharc_test.cpp ${_sharcSpirv})
target_compile_definitions(sharc_test PRIVATE SHARC_TEST_SHADER_DIR="${_sharcShaderDir}")
target_link_libraries(sharc_test PRIVATE engine_test_support)
add_test(NAME sharc COMMAND sharc_test)
# 77: no Vulkan instance or GPU
set_tests_properties(sharc PROPERTIES SKIP_RETURN_CODE 77)

# --- NRC: Windows-only binaries ---
if (WIN32)
    add_executable(nrc_library_test nrc_library_test.cpp)
    target_link_libraries(nrc_library_test PRIVATE engine_render engine_test_support)
    if (ENGINE_USE_MIMALLOC)
        target_link_libraries(nrc_library_test PRIVATE ThirdParty::mimalloc engine_mi_new_delete)
    endif ()
    add_test(NAME nrc_library COMMAND nrc_library_test)
    set_tests_properties(nrc_library PROPERTIES SKIP_RETURN_CODE 77)

    # --- DLSS: MSVC-built NGX lib linked through ngx_msvc_shim ---
    add_executable(dlss_test dlss_test.cpp)
    target_link_libraries(dlss_test PRIVATE ThirdParty::ngx engine_test_support)
    add_custom_command(TARGET dlss_test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ENGINE_DLSS_RUNTIME_DLLS} $<TARGET_FILE_DIR:dlss_test>
        COMMENT "Copying DLSS feature DLLs...")
    add_test(NAME dlss COMMAND dlss_test)
    set_tests_properties(dlss PROPERTIES SKIP_RETURN_CODE 77)
endif ()
