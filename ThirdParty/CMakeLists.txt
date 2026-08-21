# ThirdParty/CMakeLists.txt
include(FetchContent)

# Shared git clones under ThirdParty/_deps/src (survive cache wipes, not forked
# per Debug/Release). Configure/build trees live in the binary dir so two
# presets can configure at once without clobbering CMake caches.
set(ENGINE_TP_SRC "${CMAKE_CURRENT_LIST_DIR}/_deps/src")
set(ENGINE_TP_BIN "${CMAKE_BINARY_DIR}/_deps")
message(STATUS "FetchContent sources: ${ENGINE_TP_SRC}")
message(STATUS "FetchContent builds:  ${ENGINE_TP_BIN}")

macro(engine_fetch_declare name)
    FetchContent_Declare(${name}
            ${ARGN}
            SOURCE_DIR "${ENGINE_TP_SRC}/${name}"
            BINARY_DIR "${ENGINE_TP_BIN}/${name}-build"
            SUBBUILD_DIR "${ENGINE_TP_BIN}/${name}-subbuild"
    )
endmacro()

# Optimization: Disable C++ Module scanning for third-party libs
# They don't use modules yet, so scanning them is wasted time.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# ------------------------------------------------------------------------------
# Vulkan SDK (found via VULKAN_SDK env var)
# ------------------------------------------------------------------------------
if (NOT DEFINED ENV{VULKAN_SDK})
    message(FATAL_ERROR "Environment variable VULKAN_SDK must point to your Vulkan SDK root.")
endif ()

set(VULKAN_SDK_ROOT "$ENV{VULKAN_SDK}")
set(Vulkan_INCLUDE_DIR "${VULKAN_SDK_ROOT}/include" CACHE PATH "Vulkan Include Path" FORCE)

list(APPEND CMAKE_PREFIX_PATH
        "${VULKAN_SDK_ROOT}"
        "${VULKAN_SDK_ROOT}/lib/cmake"
        "${VULKAN_SDK_ROOT}/Lib/cmake"
        "${VULKAN_SDK_ROOT}/lib/cmake/Vulkan"
        "${VULKAN_SDK_ROOT}/Lib/cmake/Vulkan"
)

find_library(Vulkan_LIBRARY
        NAMES vulkan libvulkan
        PATHS "${VULKAN_SDK_ROOT}/lib"
        NO_DEFAULT_PATH
        NO_CMAKE_FIND_ROOT_PATH
)
find_package(Vulkan REQUIRED)

message(STATUS "-------------------------------------------------------------")
message(STATUS "Vulkan SDK Configuration:")
message(STATUS "  Vulkan SDK Root:    ${VULKAN_SDK_ROOT}")
message(STATUS "  Vulkan Include:     ${Vulkan_INCLUDE_DIR}")
message(STATUS "  Vulkan Library:     ${Vulkan_LIBRARY}")
message(STATUS "-------------------------------------------------------------")

set(VULKAN_COMPILE_DEFS
        VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1
        VULKAN_HPP_NO_STRUCT_CONSTRUCTORS=1
        VULKAN_HPP_NO_STD_MODULE=1
        VULKAN_HPP_NO_MODULE=1
)

message(STATUS "Using Vulkan headers (modules disabled)")
add_library(VulkanCppModule INTERFACE)
target_compile_definitions(VulkanCppModule INTERFACE ${VULKAN_COMPILE_DEFS})
target_include_directories(VulkanCppModule INTERFACE "${Vulkan_INCLUDE_DIR}")
target_link_libraries(VulkanCppModule INTERFACE Vulkan::Vulkan)

add_library(Vulkan::cppm ALIAS VulkanCppModule)

# ------------------------------------------------------------------------------
# Vulkan Memory Allocator (from Vulkan SDK)
# ------------------------------------------------------------------------------
set(VMA_HEADER_PATH "${VULKAN_SDK_ROOT}/include/vma/vk_mem_alloc.h")
if (NOT EXISTS "${VMA_HEADER_PATH}")
    message(FATAL_ERROR "VMA header not found at ${VMA_HEADER_PATH}. Ensure Vulkan SDK includes VMA.")
endif ()

message(STATUS "Using VMA from Vulkan SDK: ${VMA_HEADER_PATH}")
add_library(VulkanMemoryAllocator INTERFACE)
target_include_directories(VulkanMemoryAllocator INTERFACE "${VULKAN_SDK_ROOT}/include")
target_link_libraries(VulkanMemoryAllocator INTERFACE Vulkan::Vulkan)

# ------------------------------------------------------------------------------
# 1. Slang (Manual Download & Extract)
# ------------------------------------------------------------------------------
set(SLANG_TAG "v2026.14.1")
string(REGEX REPLACE "^v" "" SLANG_VERSION "${SLANG_TAG}")
set(SLANG_RELEASE_URL "https://github.com/shader-slang/slang/releases/download")

if (WIN32)
    set(SLANG_OS_NAME "win")
    set(SLANG_ARTIFACT_NAME "slang-${SLANG_VERSION}-windows-x86_64.zip")
elseif (UNIX AND NOT APPLE)
    set(SLANG_OS_NAME "linux")
    set(SLANG_ARTIFACT_NAME "slang-${SLANG_VERSION}-linux-x86_64.zip")
else ()
    message(FATAL_ERROR "Unsupported OS for Slang.")
endif ()

set(SLANG_DOWNLOAD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/slang_download")
set(SLANG_EXTRACT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Slang-${SLANG_OS_NAME}")
set(SLANG_DOWNLOAD_URL "${SLANG_RELEASE_URL}/${SLANG_TAG}/${SLANG_ARTIFACT_NAME}")

if (NOT EXISTS "${SLANG_EXTRACT_DIR}/README.md")
    message(STATUS "Downloading Slang from ${SLANG_DOWNLOAD_URL}")
    file(DOWNLOAD ${SLANG_DOWNLOAD_URL} "${SLANG_DOWNLOAD_DIR}/${SLANG_ARTIFACT_NAME}" SHOW_PROGRESS STATUS download_status)
    list(GET download_status 0 error_code)

    if (NOT error_code EQUAL 0)
        list(GET download_status 1 error_msg)
        message(FATAL_ERROR "Failed to download Slang: ${error_msg}")
    endif ()

    message(STATUS "Extracting Slang to ${SLANG_EXTRACT_DIR}...")
    file(ARCHIVE_EXTRACT INPUT "${SLANG_DOWNLOAD_DIR}/${SLANG_ARTIFACT_NAME}" DESTINATION "${SLANG_EXTRACT_DIR}")
    file(REMOVE_RECURSE "${SLANG_DOWNLOAD_DIR}")
endif ()

# ==============================================================================
# FetchContent — Options set BEFORE declare/populate so they are visible
# when CMake configures each sub-project during MakeAvailable.
# ==============================================================================

# --- SDL3 ---
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(SDL_VULKAN ON CACHE BOOL "" FORCE)
if (UNIX AND NOT APPLE)
    set(SDL_WAYLAND ON CACHE BOOL "" FORCE)
    set(SDL_X11 ON CACHE BOOL "" FORCE)
endif ()

# --- fmt ---
set(FMT_SHARED ON CACHE BOOL "" FORCE)
set(FMT_DOC OFF CACHE BOOL "" FORCE)
set(FMT_TEST OFF CACHE BOOL "" FORCE)

# --- spdlog ---
set(SPDLOG_BUILD_SHARED ON CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_PIC ON CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "" FORCE)

# --- Taskflow ---
set(TF_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TF_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(TF_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(TF_BUILD_PROFILER OFF CACHE BOOL "" FORCE)


# --- KTX-Software (lib only) ---
set(KTX_FEATURE_TESTS OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_TOOLS OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_DOC OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_JNI OFF CACHE BOOL "" FORCE)
set(KTX_FEATURE_PY OFF CACHE BOOL "" FORCE)
set(BASISU_SUPPORT_SSE OFF CACHE BOOL "" FORCE)
set(LIBKTX_FEATURE_VK_UPLOAD ON CACHE BOOL "" FORCE)
set(LIBKTX_FEATURE_GL_UPLOAD OFF CACHE BOOL "" FORCE)

# --- mimalloc ---
set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
set(MI_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(MI_OVERRIDE ON CACHE BOOL "" FORCE)

# --- meshoptimizer ---
set(MESHOPT_BUILD_DEMO OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_GLTFPACK OFF CACHE BOOL "" FORCE)
set(MESHOPT_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(MESHOPT_INSTALL OFF CACHE BOOL "" FORCE) # TODO explore this

# ==============================================================================
# FetchContent Declares — ALL declared before MakeAvailable so CMake can
# resolve them as a batch and avoid redundant network/disk operations.
# ==============================================================================

# 2. GLM (Header Only)
engine_fetch_declare(glm
        GIT_REPOSITORY https://github.com/g-truc/glm.git
        GIT_TAG 1.0.2
        GIT_SHALLOW TRUE
)

# 3. SDL3
engine_fetch_declare(SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.4.12
        GIT_SHALLOW TRUE
)

# 4. fmt — populated before spdlog so TARGET fmt::fmt exists (SPDLOG_FMT_EXTERNAL)
engine_fetch_declare(fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt.git
        GIT_TAG 12.2.0
        GIT_SHALLOW TRUE
)

# 5. spdlog
engine_fetch_declare(spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG v1.17.0
        GIT_SHALLOW TRUE
)

# 7. ImGui (sources only; we add_library ourselves).
# SOURCE_SUBDIR skips imgui's own CMakeLists (v1.92+) which would clash with
# our static imgui target. Feature toggle is Constants.h ENGINE_ENABLE_IMGUI.
engine_fetch_declare(ImGui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.92.8
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR skip-add-subdirectory
)

# 8. Taskflow
engine_fetch_declare(taskflow
        GIT_REPOSITORY https://github.com/taskflow/taskflow.git
        GIT_TAG v4.1.0
        GIT_SHALLOW TRUE
)

# 11. KTX-Software (library only)
engine_fetch_declare(KTXSoftware
        GIT_REPOSITORY https://github.com/KhronosGroup/KTX-Software.git
        GIT_TAG v4.4.2
        GIT_SHALLOW TRUE
)

# 12. mimalloc
engine_fetch_declare(mimalloc
        GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
        GIT_TAG v3.3.2
        GIT_SHALLOW TRUE
)

# 14. meshoptimizer (mesh optimization / clustering for GPU-driven rendering)
engine_fetch_declare(meshoptimizer
        GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
        GIT_TAG v1.2
        GIT_SHALLOW TRUE
)

# fmt before spdlog: spdlog's CMake uses TARGET fmt::fmt when SPDLOG_FMT_EXTERNAL=ON.
set(_tp_fetch_names glm SDL3 fmt spdlog ImGui taskflow KTXSoftware mimalloc meshoptimizer)
message(STATUS "Fetching ThirdParty dependencies: ${_tp_fetch_names}")
FetchContent_MakeAvailable(${_tp_fetch_names})



if (TARGET ktx)
        add_library(ThirdParty::ktx ALIAS ktx)
elseif (TARGET KTX::ktx)
        add_library(ThirdParty::ktx ALIAS KTX::ktx)
else ()
        message(FATAL_ERROR "KTX target not found after FetchContent_MakeAvailable(KTXSoftware).")
endif ()

if (TARGET mimalloc)
        add_library(ThirdParty::mimalloc ALIAS mimalloc)
elseif (TARGET mimalloc-static)
        add_library(ThirdParty::mimalloc ALIAS mimalloc-static)
else ()
        message(FATAL_ERROR "mimalloc target not found after FetchContent_MakeAvailable(mimalloc).")
endif ()

if (TARGET meshoptimizer)
        # Static meshoptimizer is linked into shared engine modules — need PIC.
        set_target_properties(meshoptimizer PROPERTIES POSITION_INDEPENDENT_CODE ON)
        add_library(ThirdParty::meshoptimizer ALIAS meshoptimizer)
else ()
        message(FATAL_ERROR "meshoptimizer target not found after FetchContent_MakeAvailable(meshoptimizer).")
endif ()

# ------------------------------------------------------------------------------
set(IMGUI_SOURCES
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        # BACKENDS
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)

# NOTE: Keeping ImGui STATIC is highly recommended even in a shared build.
# Exporting ImGui symbols across DLL boundaries requires complex configuration.
add_library(imgui STATIC ${IMGUI_SOURCES})

# UNITY_BUILD merges all 7 imgui .cpp files into one TU — ~3-4x faster compile.
# PIC required so imgui can link into shared engine libraries.
set_target_properties(imgui PROPERTIES
        UNITY_BUILD ON
        POSITION_INDEPENDENT_CODE ON
)

target_include_directories(imgui PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC
        SDL3::SDL3
        Vulkan::Vulkan
)


# ------------------------------------------------------------------------------
# 8. Tracy
# ------------------------------------------------------------------------------

# Force static Tracy client library.
set(TRACY_STATIC ON CACHE BOOL "" FORCE)

# v0.14.0 defaults TRACY_ENABLE OFF; client ABI must match engine TUs
# that define TRACY_ENABLE for non-Release.
set(TRACY_ENABLE ON CACHE BOOL "Enable Tracy profiling" FORCE)

# Tracy build-time options — must be set before MakeAvailable so Tracy's
# own CMakeLists.txt picks them up when it is first configured.
# v0.14.0: TRACY_CALLSTACK is a zone call-stack depth, not a bool.
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        # Debug: higher-fidelity capture.
        set(TRACY_ON_DEMAND   ON CACHE BOOL "Tracy: connect/profile on demand" FORCE)
        set(TRACY_CALLSTACK   16 CACHE STRING "Tracy: zone call stack depth"    FORCE)
        set(TRACY_NO_EXIT     ON CACHE BOOL "Tracy: keep client alive on exit"  FORCE)
        set(TRACY_FIBERS      ON CACHE BOOL "Tracy: fiber support"              FORCE)
        set(TRACY_VERBOSE     ON CACHE BOOL "Tracy: verbose output"             FORCE)
        set(TRACY_DEMANGLE    ON CACHE BOOL "Tracy: demangle symbol names"      FORCE)
        set(TRACY_NO_SYSTEM_TRACING ON CACHE BOOL "Tracy: disable system tracing" FORCE)
else ()
        # Lower overhead profile for Release/RelWithDebInfo/MinSizeRel.
        set(TRACY_ON_DEMAND   ON  CACHE BOOL "Tracy: connect/profile on demand" FORCE)
        set(TRACY_CALLSTACK   ""  CACHE STRING "Tracy: zone call stack depth"   FORCE)
        set(TRACY_NO_EXIT     OFF CACHE BOOL "Tracy: keep client alive on exit" FORCE)
        set(TRACY_FIBERS      OFF CACHE BOOL "Tracy: fiber support"             FORCE)
        set(TRACY_VERBOSE     OFF CACHE BOOL "Tracy: verbose output"            FORCE)
        set(TRACY_DEMANGLE    OFF CACHE BOOL "Tracy: demangle symbol names"     FORCE)
        set(TRACY_NO_SYSTEM_TRACING ON CACHE BOOL "Tracy: disable system tracing" FORCE)
endif ()

engine_fetch_declare(tracy
        GIT_REPOSITORY https://github.com/wolfpld/tracy.git
        GIT_TAG v0.14.0
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE
)
message(STATUS "Downloading Tracy...")
FetchContent_MakeAvailable(tracy)


if (WIN32)
        # Keep Windows target compatibility and required system libs for Tracy.
        target_compile_definitions(TracyClient PRIVATE WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
        target_link_libraries(TracyClient PRIVATE ws2_32 dbghelp user32)
endif ()


# ------------------------------------------------------------------------------
# Targets Definition
# ------------------------------------------------------------------------------

# 1. Headers-only interface
add_library(ThirdPartyHeaders INTERFACE)
target_include_directories(ThirdPartyHeaders INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${SLANG_EXTRACT_DIR}/include>
)
add_library(ThirdParty::headers ALIAS ThirdPartyHeaders)

# Compile it as a STATIC library
add_library(ThirdPartyImpl STATIC implementations.cpp)
# *_IMPLEMENTATION TUs must not pick up the engine PCH (guards already set).
set_source_files_properties(implementations.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

# Add tiny glTF C source to static ThirdPartyImpl
# tiny_gltf_v3.c is C code; compile it as C11 and enable stdio-backed fs helpers.
target_sources(ThirdPartyImpl PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tiny_gltf_v3.c)
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/tiny_gltf_v3.c PROPERTIES LANGUAGE C)

# Ensure C11 is used for C sources and define TINYGLTF3_ENABLE_FS for the C compilation unit.
target_compile_options(ThirdPartyImpl PRIVATE
        $<$<AND:$<COMPILE_LANGUAGE:C>,$<NOT:$<C_COMPILER_ID:MSVC>>>:-std=c11>
        $<$<AND:$<COMPILE_LANGUAGE:C>,$<C_COMPILER_ID:MSVC>>:/std:c11>
)
target_compile_definitions(ThirdPartyImpl PRIVATE
        $<$<COMPILE_LANGUAGE:C>:TINYGLTF3_ENABLE_FS>
)

# IMPORTANT: Because this static lib will be linked into a SHARED library (engine_util.dll),
# it must be compiled with Position Independent Code (PIC).
set_target_properties(ThirdPartyImpl PROPERTIES POSITION_INDEPENDENT_CODE ON)


# 2. All-in-one Interface for libs
add_library(ThirdPartyLibs INTERFACE)

target_link_libraries(ThirdPartyLibs INTERFACE
        ThirdParty::headers
        Vulkan::Vulkan
        Vulkan::cppm
        VulkanMemoryAllocator
        ThirdParty::ktx
        glm::glm
        SDL3::SDL3
        spdlog::spdlog
        $<$<NOT:$<CONFIG:Release>>:Tracy::TracyClient>
        fmt::fmt
        imgui
        ThirdParty::mimalloc
        ThirdParty::meshoptimizer
        Taskflow::Taskflow
)

# Link it to your interface so it gets the includes
target_link_libraries(ThirdPartyImpl PUBLIC ThirdPartyLibs)


add_library(ThirdParty::all ALIAS ThirdPartyLibs)
