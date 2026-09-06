cmake_minimum_required(VERSION 3.29)

# Compiler launcher must be set before project() so try_compile is cached too.
find_program(CCACHE_PROGRAM ccache HINTS "C:/msys64/clang64/bin" "C:/msys64/usr/bin")
if (CCACHE_PROGRAM)
  set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
  set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif ()

project(engine)

if (CCACHE_PROGRAM)
  message(STATUS "Using ccache: ${CCACHE_PROGRAM}")
else ()
  message(WARNING "ccache not found. Install it to speed up recompilation (e.g., 'pacman -S mingw-w64-clang-x86_64-ccache' in MSYS2).")
endif ()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(BUILD_SHARED_LIBS ON) # default; individual FetchContent libs override
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

option(ENABLE_SANITIZERS "ASan+UBSan in Debug (engine and third-party)" ON)
option(ENGINE_ENABLE_COVERAGE "Clang source-based coverage in Debug (engine targets)" ON)
option(ENGINE_UNITY_BUILD "UNITY_BUILD on engine libraries (not postprocessing)" OFF)

if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU")
  message(FATAL_ERROR "Unsupported compiler: ${CMAKE_CXX_COMPILER_ID}. This project targets Clang toolchains.")
endif ()

# Own per-config flags. Do not put -O/-g in the toolchain file — there is no
# Debug vs Release there, and FetchContent would still need the same flags.
# Directory-scope add_compile_options / add_link_options inherit into
# ThirdParty so SDL, fmt, KTX, etc. get the same -O/-g as engine targets.
foreach (_lang C CXX)
  set(CMAKE_${_lang}_FLAGS_DEBUG "")
  set(CMAKE_${_lang}_FLAGS_RELEASE "")
  set(CMAKE_${_lang}_FLAGS_RELWITHDEBINFO "")
endforeach ()

function(engine_add_config_compile_options config)
  foreach (flag IN LISTS ARGN)
    add_compile_options("$<$<CONFIG:${config}>:${flag}>")
  endforeach ()
endfunction()

function(engine_add_config_link_options config)
  foreach (flag IN LISTS ARGN)
    add_link_options("$<$<CONFIG:${config}>:${flag}>")
  endforeach ()
endfunction()

if (WIN32)
  engine_add_config_compile_options(Debug
      -O0 -g -gdwarf -gcodeview -march=native -mtune=native -pthread)
  engine_add_config_compile_options(Release
      -O3 -march=native -mtune=native -pthread -flto=thin)
  engine_add_config_compile_options(RelWithDebInfo
      -O2 -g -gdwarf -gcodeview -march=native -mtune=native -pthread)
  engine_add_config_link_options(Debug -pthread)
  engine_add_config_link_options(Release -pthread -flto=thin)
  engine_add_config_link_options(RelWithDebInfo -pthread)
  add_link_options("$<$<CONFIG:Debug,RelWithDebInfo>:-Wl,--pdb=>")
else ()
  engine_add_config_compile_options(Debug
      -ggdb -Og -march=native -mtune=native -pthread)
  engine_add_config_compile_options(Release
      -O3 -march=native -mtune=native -pthread -flto=thin)
  engine_add_config_compile_options(RelWithDebInfo
      -O2 -ggdb -march=native -mtune=native -pthread)
  engine_add_config_link_options(Debug -pthread)
  engine_add_config_link_options(Release -pthread -flto=thin)
  engine_add_config_link_options(RelWithDebInfo -pthread)
endif ()

add_compile_definitions($<$<NOT:$<CONFIG:Debug>>:NDEBUG>)

# ASan/UBSan are Debug-only. Release/RelWithDebInfo never get -fsanitize even if
# ENABLE_SANITIZERS is ON (CLion cache). Release presets force the option OFF.
if (ENABLE_SANITIZERS)
  add_compile_options("$<$<CONFIG:Debug>:-fsanitize=address,undefined>"
      "$<$<CONFIG:Debug>:-fno-omit-frame-pointer>"
      "$<$<CONFIG:Debug>:-fno-optimize-sibling-calls>")
  add_link_options("$<$<CONFIG:Debug>:-fsanitize=address,undefined>")
  message(STATUS "Sanitizers: ASan+UBSan on Debug only")
else ()
  message(STATUS "Sanitizers: OFF")
endif ()

message(STATUS "CMAKE_Version: ${CMAKE_VERSION}")
message(STATUS "CMAKE_BUILD_TYPE: ${CMAKE_BUILD_TYPE}")
message(STATUS "ENABLE_SANITIZERS: ${ENABLE_SANITIZERS}")
message(STATUS "ENGINE_ENABLE_COVERAGE: ${ENGINE_ENABLE_COVERAGE}")
message(STATUS "=== TOOLCHAIN VERIFICATION ===")
message(STATUS "C   compiler:   ${CMAKE_C_COMPILER}")
message(STATUS "CXX compiler:   ${CMAKE_CXX_COMPILER}")
message(STATUS "RC  compiler:   ${CMAKE_RC_COMPILER}")
message(STATUS "Linker:         ${CMAKE_LINKER}")
message(STATUS "Linker type:    ${CMAKE_LINKER_TYPE}")
message(STATUS "AR:             ${CMAKE_AR}")
message(STATUS "RANLIB:         ${CMAKE_RANLIB}")
message(STATUS "NM:             ${CMAKE_NM}")
message(STATUS "STRIP:          ${CMAKE_STRIP}")
message(STATUS "OBJCOPY:        ${CMAKE_OBJCOPY}")
message(STATUS "OBJDUMP:        ${CMAKE_OBJDUMP}")
message(STATUS "Make program:   ${CMAKE_MAKE_PROGRAM}")
message(STATUS "CXX flags:      ${CMAKE_CXX_FLAGS}")
message(STATUS "C flags:        ${CMAKE_C_FLAGS}")
message(STATUS "==============================")

# --- Add Dependencies (Vendored) ---
add_subdirectory(ThirdParty)

set(CMAKE_VERBOSE_MAKEFILE OFF)

# Place all runtime and library outputs in the build root so DLLs sit next to the executable
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
foreach (CONFIG DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  string(TOUPPER ${CONFIG} CONFIG_UPPER)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_BINARY_DIR})
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CONFIG_UPPER} ${CMAKE_BINARY_DIR})
endforeach ()

# --- Shader Compilation (Slang only) ---
# Include-only libraries (types/, core/, geometry/, material/, …) are not compiled.
# Compile units live in shaders/passes/ and each lists its slangc -entry names.
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/shaders)
set(ENGINE_SLANG_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/shaders")

if (WIN32)
  set(SLANGC_EXECUTABLE "${CMAKE_SOURCE_DIR}/ThirdParty/Slang-win/bin/slangc.exe")
else ()
  set(SLANGC_EXECUTABLE "${CMAKE_SOURCE_DIR}/ThirdParty/Slang-linux/bin/slangc")
endif ()

# -----------------------------------------------------------------------------
# Slang capabilities ↔ Device::requiredDeviceExtension (vk_device.hpp)
# + Vulkan 1.2–1.4 core shader features used by this engine.
#
# Only SPIR-V / shading features that slangc exposes are listed. Non-shader
# device extensions (swapchain, present, memory budget/priority, multi-draw,
# DGC, shader object, maintenance*, …) have no Slang capability and are omitted.
# -----------------------------------------------------------------------------
# Prefer SPV_* extension caps + spv* feature bits. Avoid stage meta-caps
# (meshshading / raytracing / rayquery) — they can restrict VS/FS entry points.
set(ENGINE_SLANG_CAPABILITIES
    # Baseline target
    SPIRV_1_6
    vk_mem_model

    # Vulkan 1.2 core: buffer device address / SSBO-as-pointer (BDA push / ObjectUB)
    SPV_EXT_physical_storage_buffer
    GL_EXT_buffer_reference

    # Vulkan 1.2 core: descriptor indexing (non-uniform resource indexing)
    SPV_EXT_descriptor_indexing
    spvShaderNonUniform

    # Vulkan 1.3 core: demote to helper invocation
    SPV_EXT_demote_to_helper_invocation

    # vk::KHRShaderUntypedPointersExtensionName
    SPV_KHR_untyped_pointers

    # vk::EXTDescriptorHeapExtensionName
    SPV_EXT_descriptor_heap
    spvDescriptorHeapEXT

    # vk::EXTMeshShaderExtensionName
    SPV_EXT_mesh_shader
    spvMeshShadingEXT

    # vk::KHRAccelerationStructureExtensionName + vk::KHRRayTracingPipelineExtensionName
    SPV_KHR_ray_tracing
    spvRayTracingKHR

    # vk::KHRRayQueryExtensionName
    SPV_KHR_ray_query
    spvRayQueryKHR

    # vk::EXTRayTracingInvocationReorderExtensionName
    SPV_EXT_shader_invocation_reorder
    spvShaderInvocationReorderEXT

    # vk::NVClusterAccelerationStructureExtensionName
    SPV_NV_cluster_acceleration_structure
    spvRayTracingClusterAccelerationStructureNV
)
# Join with '+' for a single slangc -capability argument.
list(JOIN ENGINE_SLANG_CAPABILITIES "+" ENGINE_SLANG_CAPABILITY_STRING)
message(STATUS "Slang shader capabilities: ${ENGINE_SLANG_CAPABILITY_STRING}")

# Debug / empty: -g2 -O0. RelWithDebInfo: -g2 -O2. Release/MinSizeRel: -g0 -O2.
# Always emit non-empty args via $<IF:...> — bare $<$<CONFIG:...>:-flag> leaves
# "" placeholders that slangc treats as inputs.
set(ENGINE_SLANG_CONFIG_FLAGS
    "$<IF:$<OR:$<CONFIG:Debug>,$<CONFIG:>>,-g2,$<IF:$<CONFIG:RelWithDebInfo>,-g2,-g0>>"
    "$<IF:$<OR:$<CONFIG:Debug>,$<CONFIG:>>,-O0,-O2>"
)

set(COMPILED_SHADERS)

function(engine_add_slang_pass rel_path)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "ENTRIES;DEPENDS")
  set(_src "${ENGINE_SLANG_INCLUDE_DIR}/${rel_path}")
  get_filename_component(_rel_dir ${rel_path} DIRECTORY)
  get_filename_component(_name_we ${_src} NAME_WE)
  if (_rel_dir STREQUAL "")
    set(_out_dir "${CMAKE_BINARY_DIR}/shaders")
  else ()
    set(_out_dir "${CMAKE_BINARY_DIR}/shaders/${_rel_dir}")
  endif ()
  set(_spv "${_out_dir}/${_name_we}.spv")

  set(_entry_args)
  foreach (_e IN LISTS ARG_ENTRIES)
    list(APPEND _entry_args -entry ${_e})
  endforeach ()

  set(_depends "${_src}")
  foreach (_d IN LISTS ARG_DEPENDS)
    list(APPEND _depends "${ENGINE_SLANG_INCLUDE_DIR}/${_d}")
  endforeach ()

  add_custom_command(
      OUTPUT ${_spv}
      COMMAND ${CMAKE_COMMAND} -E make_directory ${_out_dir}
      COMMAND ${SLANGC_EXECUTABLE}
      ${_src}
      -target spirv
      -profile sm_6_9
      -capability ${ENGINE_SLANG_CAPABILITY_STRING}
      -matrix-layout-column-major
      -fvk-use-entrypoint-name
      -fvk-use-scalar-layout
      -I ${ENGINE_SLANG_INCLUDE_DIR}
      ${ENGINE_SLANG_CONFIG_FLAGS}
      ${_entry_args}
      -o ${_spv}
      DEPENDS ${_depends}
      COMMENT "Compiling Slang shader ${rel_path}"
      VERBATIM
  )
  set(COMPILED_SHADERS ${_spv} ${COMPILED_SHADERS} PARENT_SCOPE)
endfunction()

engine_add_slang_pass(passes/mesh_raster.slang
    ENTRIES meshMain
    DEPENDS types.slang core/binding.slang core/math.slang geometry/mesh.slang)
engine_add_slang_pass(passes/mesh_fragment.slang
    ENTRIES fragMain
    DEPENDS types.slang core/binding.slang material/pbr.slang)

add_custom_target(Shaders ALL DEPENDS ${COMPILED_SHADERS})

# --- Paths Definitions ---
file(TO_CMAKE_PATH "${CMAKE_BINARY_DIR}/shaders" ENGINE_SHADER_DIR_PATH)
file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/models" ENGINE_MODELS_DIR_PATH)
file(TO_CMAKE_PATH "${CMAKE_SOURCE_DIR}/textures" ENGINE_TEXTURES_DIR_PATH)

add_subdirectory(src)
