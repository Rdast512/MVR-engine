#pragma once
// everything included here will be seen by all source files in the project, and will be precompiled into a single header for faster compilation times
// Standard Library
#include <vector>
#include <string>
#include <memory>
#include <optional>
#include <iostream>
#include <stdexcept>
#include <cstdlib>
#include <map>
#include <unordered_map>
#include <ranges>
#include <fstream>
#include <filesystem>
#include <cstdint>
#include <cassert>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

// Third Party
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#define GLM_ENABLE_EXPERIMENTAL
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/hash.hpp>
#include <glm/gtc/type_ptr.hpp>



#define VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE 1
#define VULKAN_HPP_NO_STD_MODULE
#include <vulkan/vulkan_raii.hpp>
#include <vulkan/vk_platform.h>
#include "vma/vk_mem_alloc.h"
#include "tiny_obj_loader.h"
#include "tiny_gltf_v3.h"
#include "termcolor.hpp"
#include "mimalloc.h"
#include "stb_image.h"