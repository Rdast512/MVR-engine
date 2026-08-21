# Compilers, libc++, LLD. Optimization and debug-info flags are NOT set here —
# they live in the top-level CMakeLists.txt as directory-scope generator
# expressions so engine targets and FetchContent libs share the same -O/-g.
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_AR llvm-ar)
set(CMAKE_RANLIB llvm-ranlib)
set(CMAKE_NM llvm-nm)
set(CMAKE_LINKER_TYPE LLD)

add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>)
add_link_options(-stdlib=libc++)
