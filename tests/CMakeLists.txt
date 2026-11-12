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

# --- Hi-Z: screen footprint of known spheres (shaders/core/culling.slang) ---
set(_hizShader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/hiz_footprint.slang")
set(_hizShaderDir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
# one module per depth convention (core/culling.slang CULLING_REVERSED_Z)
set(_hizSpirvs)
foreach (_convention IN ITEMS Standard Reversed)
  if (_convention STREQUAL "Reversed")
    set(_reversed 1)
  else ()
    set(_reversed 0)
  endif ()
  set(_spirv "${_hizShaderDir}/footprint${_convention}.spv")
  add_custom_command(
      OUTPUT ${_spirv}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${_hizShaderDir}
      COMMAND ${SLANGC_EXECUTABLE} ${_hizShader} ${ENGINE_SLANG_FLAGS} -DCULLING_REVERSED_Z=${_reversed}
              -entry footprintMain -depfile ${_spirv}.d -o ${_spirv}
      DEPENDS ${_hizShader}
      DEPFILE ${_spirv}.d
      COMMENT "Compiling Hi-Z test shader footprintMain (${_convention} Z)"
      VERBATIM
  )
  list(APPEND _hizSpirvs ${_spirv})
endforeach ()

add_executable(hiz_footprint_test hiz_footprint_test.cpp ${_hizSpirvs})
target_compile_definitions(hiz_footprint_test PRIVATE HIZ_TEST_SHADER_DIR="${_hizShaderDir}")
target_link_libraries(hiz_footprint_test PRIVATE engine_test_support)
add_test(NAME hiz_footprint COMMAND hiz_footprint_test)
set_tests_properties(hiz_footprint PROPERTIES SKIP_RETURN_CODE 77)

# --- BRDF: worked numbers and sampler consistency (shaders/material/brdf.slang) ---
set(_brdfShader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/brdf_numbers.slang")
set(_brdfShaderDir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
set(_brdfSpirv "${_brdfShaderDir}/brdfMain.spv")
add_custom_command(
    OUTPUT ${_brdfSpirv}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_brdfShaderDir}
    COMMAND ${SLANGC_EXECUTABLE} ${_brdfShader} ${ENGINE_SLANG_FLAGS} -entry brdfMain -depfile ${_brdfSpirv}.d
            -o ${_brdfSpirv}
    DEPENDS ${_brdfShader}
    DEPFILE ${_brdfSpirv}.d
    COMMENT "Compiling BRDF test shader brdfMain"
    VERBATIM
)

add_executable(brdf_test brdf_test.cpp ${_brdfSpirv})
target_compile_definitions(brdf_test PRIVATE BRDF_TEST_SHADER_DIR="${_brdfShaderDir}")
target_link_libraries(brdf_test PRIVATE engine_test_support)
add_test(NAME brdf COMMAND brdf_test)
set_tests_properties(brdf PROPERTIES SKIP_RETURN_CODE 77)

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
