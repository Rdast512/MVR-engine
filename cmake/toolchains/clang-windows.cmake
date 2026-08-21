# Compilers, libc++, LLD, GNU triplet. Optimization and debug-info flags are NOT
# set here — they live in the top-level CMakeLists.txt as directory-scope
# generator expressions so engine targets and FetchContent libs share the same -O/-g.
set(MSYS2_ROOT "C:/msys64")

add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-stdlib=libc++>)
add_link_options(-stdlib=libc++)

set(CMAKE_C_COMPILER "${MSYS2_ROOT}/clang64/bin/clang.exe")
set(CMAKE_CXX_COMPILER "${MSYS2_ROOT}/clang64/bin/clang++.exe")
set(CMAKE_RC_COMPILER "${MSYS2_ROOT}/clang64/bin/llvm-rc.exe")

set(CMAKE_AR "${MSYS2_ROOT}/clang64/bin/llvm-ar.exe")
set(CMAKE_RANLIB "${MSYS2_ROOT}/clang64/bin/llvm-ranlib.exe")
set(CMAKE_NM "${MSYS2_ROOT}/clang64/bin/llvm-nm.exe")
set(CMAKE_STRIP "${MSYS2_ROOT}/clang64/bin/llvm-strip.exe")
set(CMAKE_OBJCOPY "${MSYS2_ROOT}/clang64/bin/llvm-objcopy.exe")
set(CMAKE_OBJDUMP "${MSYS2_ROOT}/clang64/bin/llvm-objdump.exe")

set(CMAKE_LINKER_TYPE LLD)
set(CMAKE_MAKE_PROGRAM "${MSYS2_ROOT}/clang64/bin/ninja.exe")

set(CMAKE_C_FLAGS "-target x86_64-w64-windows-gnu" CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS "-target x86_64-w64-windows-gnu" CACHE STRING "" FORCE)
