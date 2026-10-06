# gltf_ktx2: converts the PNG/JPG images a .gltf references to BC7 KTX2 and writes a copy of the
# .gltf that points at them (see main.cpp). Uses the engine's libktx (Basis encoder + transcoder).
add_executable(gltf_ktx2 main.cpp)
target_compile_features(gltf_ktx2 PRIVATE cxx_std_23)
# vulkan_core.h only for VkFormat values; no Vulkan loader link
target_include_directories(gltf_ktx2 PRIVATE "${CMAKE_SOURCE_DIR}/ThirdParty" "${Vulkan_INCLUDE_DIR}")
target_link_libraries(gltf_ktx2 PRIVATE ThirdParty::ktx)

# libktx is a DLL/.so; put it next to the tool so it runs from the build dir
get_target_property(_ktxType ktx TYPE)
if (_ktxType STREQUAL "SHARED_LIBRARY")
    add_custom_command(TARGET gltf_ktx2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ktx> $<TARGET_FILE_DIR:gltf_ktx2>
        COMMENT "Copying libktx for gltf_ktx2...")
endif ()
