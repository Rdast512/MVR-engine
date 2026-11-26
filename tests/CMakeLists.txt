add_library(engine_test_support STATIC test_support.cpp)
target_include_directories(engine_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# Vulkan::Vulkan is imported in ThirdParty scope only; the cppm wrapper is global
target_link_libraries(engine_test_support PUBLIC Vulkan::cppm)

# --- Hi-Z: screen footprint of known spheres (shaders/core/culling.slang) ---
set(_hizShader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/hiz_footprint.slang")
set(_hizShaderDir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
set(_hizSpirv "${_hizShaderDir}/footprintMain.spv")
add_custom_command(
    OUTPUT ${_hizSpirv}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_hizShaderDir}
    COMMAND ${SLANGC_EXECUTABLE} ${_hizShader} ${ENGINE_SLANG_FLAGS} -entry footprintMain -depfile ${_hizSpirv}.d
            -o ${_hizSpirv}
    DEPENDS ${_hizShader}
    DEPFILE ${_hizSpirv}.d
    COMMENT "Compiling Hi-Z test shader footprintMain"
    VERBATIM
)

add_executable(hiz_footprint_test hiz_footprint_test.cpp ${_hizSpirv})
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

# --- Lights: KHR_lights_punctual attenuation, range window and spot cone (shaders/lighting/lights.slang) ---
set(_lightsShader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/lights_numbers.slang")
set(_lightsSpirv "${_brdfShaderDir}/lightsMain.spv")
add_custom_command(
    OUTPUT ${_lightsSpirv}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_brdfShaderDir}
    COMMAND ${SLANGC_EXECUTABLE} ${_lightsShader} ${ENGINE_SLANG_FLAGS} -entry lightsMain -depfile ${_lightsSpirv}.d
            -o ${_lightsSpirv}
    DEPENDS ${_lightsShader}
    DEPFILE ${_lightsSpirv}.d
    COMMENT "Compiling lights test shader lightsMain"
    VERBATIM
)

add_executable(lights_test lights_test.cpp ${_lightsSpirv})
target_compile_definitions(lights_test PRIVATE LIGHTS_TEST_SHADER_DIR="${_brdfShaderDir}")
target_link_libraries(lights_test PRIVATE engine_test_support)
add_test(NAME lights COMMAND lights_test)
set_tests_properties(lights PROPERTIES SKIP_RETURN_CODE 77)

# --- RTXDI math: LogLuv GI reservoir packing, Jacobian bounds, shadeGiSample and CheckedBuffer ---
set(_rtxdiMathShader "${CMAKE_CURRENT_SOURCE_DIR}/shaders/rtxdi_math.slang")
set(_rtxdiMathSpirv "${_brdfShaderDir}/rtxdiMathMain.spv")
add_custom_command(
    OUTPUT ${_rtxdiMathSpirv}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${_brdfShaderDir}
    COMMAND ${SLANGC_EXECUTABLE} ${_rtxdiMathShader} ${ENGINE_SLANG_FLAGS} ${RTXDI_SLANG_FLAGS} -entry rtxdiMathMain
            -depfile ${_rtxdiMathSpirv}.d -o ${_rtxdiMathSpirv}
    DEPENDS ${_rtxdiMathShader}
    DEPFILE ${_rtxdiMathSpirv}.d
    COMMENT "Compiling RTXDI math test shader rtxdiMathMain"
    VERBATIM
)

add_executable(rtxdi_math_test rtxdi_math_test.cpp ${_rtxdiMathSpirv})
target_compile_definitions(rtxdi_math_test PRIVATE RTXDI_MATH_TEST_SHADER_DIR="${_brdfShaderDir}")
target_link_libraries(rtxdi_math_test PRIVATE engine_test_support)
add_test(NAME rtxdi_math COMMAND rtxdi_math_test)
set_tests_properties(rtxdi_math PROPERTIES SKIP_RETURN_CODE 77)

# --- Layout: shaders/types.slang reflected by the Slang library vs the C++ mirrors (src/core/types.hpp) ---
if (WIN32)
    set(_slangPackageDir "${CMAKE_SOURCE_DIR}/ThirdParty/Slang-win")
else ()
    set(_slangPackageDir "${CMAKE_SOURCE_DIR}/ThirdParty/Slang-linux")
endif ()
find_package(slang CONFIG REQUIRED PATHS ${_slangPackageDir} NO_DEFAULT_PATH)

add_executable(layout_test layout_test.cpp)
set(_rtxdiIncludeDir ${ENGINE_SLANG_THIRDPARTY_INCLUDE_DIRS})
list(FILTER _rtxdiIncludeDir INCLUDE REGEX "rtxdi")
set(_nrcIncludeDir ${ENGINE_SLANG_THIRDPARTY_INCLUDE_DIRS})
list(FILTER _nrcIncludeDir INCLUDE REGEX "nrc")
target_compile_definitions(layout_test PRIVATE ENGINE_SHADER_SOURCE_DIR="${ENGINE_SLANG_INCLUDE_DIR}"
                                               ENGINE_RTXDI_INCLUDE_DIR="${_rtxdiIncludeDir}"
                                               ENGINE_NRC_INCLUDE_DIR="${_nrcIncludeDir}")
target_link_libraries(layout_test PRIVATE engine_build_config ThirdParty::all engine_test_support slang::slang)
# GpuNrcConstants holds the SDK's NrcConstants
target_link_libraries(layout_test PRIVATE ThirdParty::nrc)
target_precompile_headers(layout_test PRIVATE ${CMAKE_SOURCE_DIR}/src/pch.h)
if (ENGINE_USE_MIMALLOC)
    # engine_build_config wraps malloc
    target_link_libraries(layout_test PRIVATE ThirdParty::mimalloc engine_mi_new_delete)
endif ()
if (WIN32)
    add_custom_command(TARGET layout_test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:slang::slang> $<TARGET_FILE_DIR:layout_test>
        COMMENT "Copying Slang runtime for layout_test")
endif ()
add_test(NAME layout COMMAND layout_test)

# --- Settings lab: registry round trips and built-in plans (spec/features/planned/full_testing.md T1, T4), CPU only ---
if (ENGINE_BUILD_CHECKS)
    add_executable(lab_registry_test lab_registry_test.cpp)
    target_link_libraries(lab_registry_test PRIVATE engine_checks engine_test_support)
    # engine headers rely on the engine's precompiled header
    target_precompile_headers(lab_registry_test PRIVATE ${CMAKE_SOURCE_DIR}/src/pch.h)
    if (ENGINE_USE_MIMALLOC)
        target_link_libraries(lab_registry_test PRIVATE ThirdParty::mimalloc engine_mi_new_delete)
    endif ()
    add_test(NAME lab_registry COMMAND lab_registry_test)
endif ()

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
