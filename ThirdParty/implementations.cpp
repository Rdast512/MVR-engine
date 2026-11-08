// single header files go here for their implementation will work. Owerwise it wont compile. Still need to include the headers in src files for usage of libs.
#define VMA_MAKE_LEAK_LOG
#define VMA_LEAK_LOG_FORMAT(format, ...) \
    do { printf((format), __VA_ARGS__); printf("\n"); } while(false)
#define VMA_VULKAN_VERSION 1004000
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION
#include "vma/vk_mem_alloc.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define TINYGLTF3_IMPLEMENTATION
#define TINYGLTF3_ENABLE_FS
#define TINYGLTF3_JSON_SIMD_AVX2
#define TINYGLTF3_ENABLE_STB_IMAGE   // enable image decoding
#define TINYGLTF3_ENABLE_STB_IMAGE_WRITE // enable image writing
// #define TINYGLTF_NO_INCLUDE_STB_IMAGE
// #define TINYGLTF_NO_INCLUDE_STB_IMAGE_WRITE
// #define TINYGLTF_USE_RAPIDJSON
// #define TINYGLTF_NO_INCLUDE_RAPIDJSON
// rapidjson must be included manually when TINYGLTF_NO_INCLUDE_RAPIDJSON is defined
// (tinygltf's own includes use bare "document.h" without the rapidjson/ prefix)
// #include <rapidjson/document.h>
// #include <rapidjson/prettywriter.h>
// #include <rapidjson/rapidjson.h>
// #include <rapidjson/stringbuffer.h>
// #include <rapidjson/writer.h>
#include "tiny_gltf_v3.h"
#include "tinygltf_json_c.h"
#include "tinygltf_json.h"