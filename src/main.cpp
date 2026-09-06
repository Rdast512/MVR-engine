//
// Created by rdast on 21.08.2025.
//

#include "./core/vk_engine.hpp"
#include "./util/debug.hpp"
#include <cstdlib>
#include <iostream>
#include <mimalloc.h>
#include <vector>

// TODO add support for GLTF and KTX2
// TODO Pipeline cache
// TODO (createVertexBuffer, createIndexBuffer, createTextureImage) are still using the "single-time command" pattern

void CheckSTL() {
    std::cout << "--------------------------------------------------\n";
    std::cout << "Build Configuration Check:\n";

    // 1. Check Compiler
    #if defined(__clang__)
        std::cout << "  Compiler: Clang " << __clang_major__ << "." << __clang_minor__ << "." << __clang_patchlevel__ << "\n";
    #elif defined(__GNUC__)
        std::cout << "  Compiler: GCC " << __GNUC__ << "." << __GNUC_MINOR__ << "\n";
    #elif defined(_MSC_VER)
        std::cout << "  Compiler: MSVC " << _MSC_VER << "\n";
    #endif

    // 2. Check Standard Library (STL)
    #if defined(_LIBCPP_VERSION)
        std::cout << "  STL:      LLVM libc++ (Version: " << _LIBCPP_VERSION << ")\n";
    #elif defined(__GLIBCXX__)
        std::cout << "  STL:      GNU libstdc++ (Date: " << __GLIBCXX__ << ")\n";
    #elif defined(_MSVC_STL_VERSION)
        std::cout << "  STL:      Microsoft STL (Version: " << _MSVC_STL_VERSION << ")\n";
    #else
        std::cout << "  STL:      Unknown\n";
    #endif

    std::cout << "--------------------------------------------------\n";
}

static void requireMimalloc(const void* p, const char* what)
{
    if (p == nullptr || !mi_is_in_heap_region(p)) {
        std::cerr << "mimalloc override failed: " << what << '\n';
        std::abort();
    }
}

int main() {
    (void)mi_version();

    void* direct = mi_malloc(64);
    requireMimalloc(direct, "mi_malloc");
    mi_free(direct);

    void* cHeap = std::malloc(64);
    requireMimalloc(cHeap, "malloc");
    std::free(cHeap);

    auto* cxxHeap = new char[64];
    requireMimalloc(cxxHeap, "new[]");
    delete[] cxxHeap;

    const std::vector<char> bytes(256);
    requireMimalloc(bytes.data(), "std::vector");

    if (!checkMimallocHeap()) {
        std::cerr << "mimalloc override failed: engine_util.dll\n";
        return EXIT_FAILURE;
    }

    CheckSTL();
    try {
        Engine engine;
        engine.run();
    } catch (const std::exception &e) {
        std::cout << "Engine ended. Exception: " << e.what() << std::endl;
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
