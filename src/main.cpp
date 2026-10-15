#include "./core/vk_engine.hpp"
#include "./util/debug.hpp"
#include <cstdlib>
#include <iostream>
#include <span>
#include <vector>
#if ENGINE_USE_MIMALLOC
#include <mimalloc.h>
#endif

namespace
{
void checkStl()
{
    std::cout << "--------------------------------------------------\n";
    std::cout << "Build Configuration Check:\n";

#if defined(__clang__)
    std::cout << "  Compiler: Clang " << __clang_major__ << "." << __clang_minor__ << "." << __clang_patchlevel__
              << "\n";
#elif defined(__GNUC__)
    std::cout << "  Compiler: GCC " << __GNUC__ << "." << __GNUC_MINOR__ << "\n";
#elif defined(_MSC_VER)
    std::cout << "  Compiler: MSVC " << _MSC_VER << "\n";
#endif

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

#if ENGINE_USE_MIMALLOC
void requireMimalloc(const void* ptr, const char* what)
{
    if (ptr == nullptr || !mi_is_in_heap_region(ptr)) {
        std::cerr << "mimalloc override failed: " << what << '\n';
        std::abort();
    }
}
#endif
} // namespace

int main([[maybe_unused]] int argc, [[maybe_unused]] char** argv)
{
#if ENGINE_USE_MIMALLOC
    (void)mi_version();

    void* direct = mi_malloc(64);
    requireMimalloc(direct, "mi_malloc");
    mi_free(direct);

    void* cHeap = std::malloc(64);
    requireMimalloc(cHeap, "malloc");
    std::free(cHeap);

    // deliberate raw new: proves the global operator new override
    auto* cxxHeap = new char[64];
    requireMimalloc(cxxHeap, "new[]");
    delete[] cxxHeap;

    const std::vector<char> bytes(256);
    requireMimalloc(bytes.data(), "std::vector");

    if (!checkMimallocHeap()) {
        std::cerr << "mimalloc override failed: engine_util.dll\n";
        return EXIT_FAILURE;
    }
#endif

    checkStl();
    try {
        Engine engine;
        engine.run();
    } catch (const std::exception& e) {
        std::cout << "Engine ended. Exception: " << e.what() << '\n';
        std::cerr << e.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
