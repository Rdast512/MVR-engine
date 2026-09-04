//
// Created by rdast on 21.08.2025.
//

#include "./core/vk_engine.hpp"
#include <iostream>
#include <mimalloc.h>

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

int main() {
    // Ensure mimalloc symbols are referenced so allocator override is loaded.
    mi_stats_reset();  // only works if mimalloc is linked

    // Allocate and verify it comes from mimalloc
    void* p = mi_malloc(64);
    assert(mi_is_in_heap_region(p));  // true if mimalloc owns this pointer
    mi_free(p);

    mi_stats_print(nullptr);  // prints to stderr
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
