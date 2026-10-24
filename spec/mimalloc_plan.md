# Plan: route CPU heap through mimalloc v3

**Status:** implemented (shared mimalloc, per-DSO new/delete + `--wrap`, stb/tinygltf `mi_*` hooks)
**Date:** 2026-09-06
**Scope:** replace process-heap C/C++ allocation (`malloc` / `free` / `new` / `delete` / STL containers) with the already-fetched **mimalloc v3.3.2**. One heap for the whole process.

**Related:** `ThirdParty/CMakeLists.txt` (FetchContent `mimalloc` `GIT_TAG v3.3.2`), `src/main.cpp`, `src/pch.h`, `src/CMakeLists.txt`, `ThirdParty/implementations.cpp`, mimalloc headers under `ThirdParty/_deps/src/mimalloc/include/`

---

## 0. Goals and non-goals

### Goals

1. **One mimalloc heap** for all engine modules (`engine.exe` + `engine_*.dll`). Cross-DLL `new`/`delete` and `malloc`/`free` stay on the same heap.
2. **C++ default allocation goes through mimalloc:** `operator new` / `operator delete` (sized + aligned + nothrow) so `std::vector`, `std::string`, `std::make_unique`, `std::make_shared` land in mimalloc without rewriting every container.
3. **C heap goes through mimalloc** where the toolchain allows: `malloc` / `calloc` / `realloc` / `free` / aligned variants. That covers `ThirdPartyImpl` (`stb_image`, `tiny_gltf_v3`, `tiny_obj_loader`) and any engine C calls.
4. Keep **RAII**. Do not introduce naked `mi_malloc` in engine C++ in place of `unique_ptr` / containers. Override the global operators; keep existing `std::vector` / `std::string`.
5. Keep the existing FetchContent pin (`v3.3.2`). Do not re-download a different tag unless a build bug requires it.

### Non-goals

- Vulkan / VMA allocations (`VkAllocator`, device-local SSBOs, staging, images, descriptor-heap buffers). Those are GPU (or GPU-mapped) memory. Tracy GPU pools stay as they are.
- Host-visible mapped Vulkan (camera UBO, instance UBO, DGC sequences, descriptor heaps). Those count as process RSS but are not CRT heap. Out of scope.
- Rewriting every `std::vector<T>` to `std::vector<T, mi_stl_allocator<T>>`. That is a fallback if global `new` cannot be overridden, not the default.
- First-class mimalloc heaps per store (`mi_heap_new` for geometry vs materials). v3 supports it; not needed to replace the CRT.
- Collapsing shared engine modules to static. `BUILD_SHARED_LIBS` stays ON.
- Changing KTX, SDL, spdlog, fmt, meshoptimizer, Tracy source. They keep their own allocators unless the process-wide `malloc` override actually intercepts them.
- Descriptor-heap DEVICE_LOCAL work, sanitizer policy in Release, PCH removal.

### Success criteria

| Area | Done when |
|---|---|
| Link | one `mimalloc` shared library; every `engine_*` DLL + `engine.exe` imports it; no static mimalloc copied into each DLL |
| C++ | `std::vector<char>(64)` data pointer is `mi_is_in_heap_region`; `new char[64]` likewise |
| C | `malloc(64)` from `engine_core` / `ThirdPartyImpl` is `mi_is_in_heap_region` (or documented as unreachable on this toolchain — see §3) |
| Cross-DLL | object allocated in `engine_core.dll`, freed in `engine_runtime.dll` (or the reverse) does not mix heaps |
| GPU | VMA create/destroy path unchanged; no `mi_*` on `VkDeviceMemory` |
| Debug | ASan still works on Debug (see §6); Release still has no sanitizers |
| LTO | Release ThinLTO still links (`MI_USE_CXX` stays ON; no duplicate `std::get_new_handler`) |
| Stats | `MIMALLOC_SHOW_STATS=1` / `mi_stats_print` shows real traffic, not only the 64-byte smoke `mi_malloc` in `main` |

---

## 1. Why (current vs target)

### Now

mimalloc is **already in the tree** and linked, but it is not the process allocator.

| Knob | Value today |
|---|---|
| Fetch | `GIT_TAG v3.3.2` |
| `MI_BUILD_STATIC` | ON |
| `MI_BUILD_SHARED` | OFF |
| `MI_OVERRIDE` | ON |
| `MI_USE_CXX` | ON (ThinLTO + libc++: C stub of `_ZSt15get_new_handler` collided) |
| Link | `ThirdParty::mimalloc` on `ThirdPartyLibs` → every engine shared lib |
| Headers | `src/pch.h` and `src/main.cpp` include `mimalloc.h` |
| Runtime | `main` calls `mi_stats_reset`, `mi_malloc(64)`, `mi_is_in_heap_region`, `mi_free`, `mi_stats_print` |

That smoke test only proves **direct `mi_malloc`** is linked. It does **not** prove `std::vector`, `new`, or `stb` `malloc` use mimalloc.

**Static mimalloc + `BUILD_SHARED_LIBS ON` is the real bug.** `engine_core`, `engine_render`, `engine_scene`, `engine_util`, `engine_postprocessing`, `engine_runtime` each link `mimalloc-static`. A static allocator has process-wide TLS/arenas **per link unit**. Allocate in one DLL, free in another → two heaps → corruption. Even if each DLL only frees its own pointers, you still pay six copies of arenas and you cannot treat the process as one allocator.

`MI_OVERRIDE ON` on a static lib also does not intercept `libc++.dll` / UCRT `malloc`. libc++ `std::allocator` calls `::operator new`, which on this toolchain (`-stdlib=libc++`, `x86_64-w64-windows-gnu`) lives in `libc++.dll` and then calls CRT `malloc`. Engine `new` in a DLL is typically an import of `_Znwm` from libc++, not a call into the static mimalloc sitting inside that DLL.

Toolchain: MSYS2 Clang, GNU Windows target, LLD, libc++. This is **not** MSVC `/MD`. `mimalloc-redirect.dll` (IAT patch of `ucrtbase.dll`) is the MSVC recipe. It is a maybe on clang64 UCRT, not a given. Plan for GNU-style override first; treat redirect as an optional extra if we later prove UCRT IAT hooking works.

### Target

```text
TODAY
  std::vector / new  ──► libc++ operator new ──► UCRT/msvcrt malloc
  stb / tinygltf     ──► malloc
  main mi_malloc     ──► mimalloc-static (copy inside engine.exe)
  engine_core new    ──► libc++  (or a *different* mimalloc-static if override leaked)

TARGET
  std::vector / new  ──► mimalloc.dll operator new / mi_new
  malloc / free      ──► mimalloc.dll (alias or wrap)
  stb / tinygltf     ──► same malloc  (or explicit STBI_/TINYGLTF3_ malloc hooks)
  VMA / VkDeviceMemory unchanged
```

---

## 2. What “replace standard C++ memory operations” means here

C++ Core Guidelines for this repo: no naked `new`/`delete`, no `malloc`/`free` in engine C++. Replacement is **not** sprinkling `mi_malloc` through `GeometryStore`. It is:

| Mechanism | Replaces | Use |
|---|---|---|
| Global `operator new/delete` (`mimalloc-new-delete.h` **or** mimalloc compiled with `MI_OVERRIDE` + `MI_USE_CXX`) | `new`, `delete`, `new[]`, sized/aligned/nothrow | **Primary.** STL and `make_unique` go through this. |
| Process `malloc` override (`MI_MALLOC_OVERRIDE` / `--wrap` / redirect) | CRT `malloc` family | **Primary for C.** stb, tinygltf, SDL internals if intercepted. |
| `mimalloc-override.h` macros | `malloc` → `mi_malloc` at preprocess time | **Only** in TUs we compile (`ThirdPartyImpl`, maybe pch). Does not affect prebuilt SDL/fmt/spdlog/ktx. Mixing with real CRT `malloc` is a heap-mix bug. Prefer symbol override over macros. |
| `mi_stl_allocator<T>` | per-container `std::allocator` | **Fallback** if global `new` cannot be replaced for a given DLL. Do not template-bomb the codebase unless fallback is required. |
| `mi_heap_*` / `mi_heap_stl_allocator` | isolated heaps | **Out of scope.** |

`mi_new` (used by the new-delete header) implements C++ OOM: `std::get_new_handler` then `std::bad_alloc`. That is why `MI_USE_CXX` must stay ON.

---

## 3. Override strategy on this toolchain

Platform is **Windows + Clang GNU + libc++ + multiple DLLs**. Pick the path that gives **one heap**, then add C `malloc` if the first path does not already capture it.

### 3.1 Shared mimalloc (required)

Flip FetchContent knobs:

```text
MI_BUILD_SHARED  ON
MI_BUILD_STATIC  OFF
MI_BUILD_OBJECT  OFF
MI_OVERRIDE      ON
MI_USE_CXX       ON     # keep; ThinLTO + libc++
MI_WIN_REDIRECT  OFF    # default ON is MSVC; do not assume redirect on clang64
```

Alias `ThirdParty::mimalloc` to the **shared** target (`mimalloc`, not `mimalloc-static`). PIC is implied for a DLL.

Copy `mimalloc.dll` next to `engine.exe` in the existing Windows `POST_BUILD` DLL copy list (`src/CMakeLists.txt` `SHARED_LIBS_TO_COPY`). Linux: copy/symlink `libmimalloc.so*` like SDL/fmt.

Link order: `engine.exe` must load mimalloc **early**. Keep a real `mi_*` call in `main` (`mi_version()` is enough; the current `mi_malloc` smoke can stay as an assert). That forces the import.

Do **not** statically link mimalloc into each `engine_*` DLL.

### 3.2 C++ `new`/`delete` (required)

mimalloc compiled as C++ with `MI_OVERRIDE` already defines `operator new/delete` inside the mimalloc DSO (`src/alloc-override.c` when `MI_USE_CXX`). That only helps TUs that **bind** those symbols to mimalloc instead of libc++.

Windows DLLs do not get ELF-style interposition for free. Each `engine_*.dll` that emits `_Znwm` will import it from **whoever it was linked against**.

**Do this:**

1. Link every engine shared lib **and** the exe to `ThirdParty::mimalloc` (already true via `ThirdParty::all`).
2. Force those modules to resolve `operator new` from mimalloc, not libc++. On LLD/GNU that is link order + not pulling libc++’s new first. If a DLL still imports `_Znwm` from `libc++.dll` (dumpbin/`llvm-nm`/`llvm-readobj` on the DLL), then:
   - include `mimalloc-new-delete.h` in **exactly one** TU **per** shared module that still imports libc++ new, **or**
   - one small `engine_alloc` object compiled into each module (same header, still one definition per DSO — Windows replaceable functions are per-module).
3. **Never** put `mimalloc-new-delete.h` in `pch.h`. The header’s contract is one TU. PCH would inject it into every source and duplicate symbols inside a DLL.

`src/pch.h` may keep `#include <mimalloc.h>` for `mi_*` APIs. That does not override `new`.

### 3.3 C `malloc` (required if 3.2 does not cover C)

`alloc-override.c` only emits `malloc` aliases when `MI_MALLOC_OVERRIDE && !defined(_DLL)`. clang64 often defines `_DLL` (shared CRT). If it does, **static/shared mimalloc will not export `malloc` at all** and stb/tinygltf keep using UCRT.

Probe (clang64, `libmimalloc-asan-debug.dll` / `libmimalloc.dll`):

| Symbol | mimalloc DSO | Action taken |
|---|---|---|
| `malloc` / `free` | **not exported** (`_DLL` path) | `--wrap=malloc,calloc,realloc,free,aligned_alloc` + `__wrap_*` in `engine_mi_new_delete` |
| `mi_malloc` / `mi_new` | exported | STL/`new` go through `mimalloc-new-delete.h` per DSO |
| ThirdPartyImpl C heap | n/a | `STBI_*` / `TINYGLTF3_*` / `TINYGLTF_JSON_*` → `mi_*` |

SDL3, fmt, spdlog, ktx are their own DLLs. Without UCRT redirect they keep CRT malloc. Accept that unless redirect is proven. Do not rebuild those libs with mimalloc macros (KTX is explicitly out of touch).

### 3.4 Rejected defaults

| Idea | Why not |
|---|---|
| `mimalloc-override.h` in `pch.h` | macros in every engine TU; still two heaps if any DLL uses real `malloc`; breaks any header that token-pastes `malloc` |
| `mi_stl_allocator` on every `std::vector` | huge diff, ABI/type churn, does not catch `std::string` / `make_unique` / third-party |
| keep static mimalloc, “only call `mi_*` in new code” | existing STL never moves; cross-DLL static copies remain |
| MSVC `mimalloc-redirect.dll` as the only Windows plan | we are not MSVC `/MD`; redirect may no-op on GNU |

---

## 4. What stays off mimalloc

| Allocator | Owner | Why |
|---|---|---|
| VMA `vmaCreateBuffer` / `vmaCreateImage` | `VkAllocator` | GPU / mapped GPU. Wrong API. |
| Descriptor heaps (host-visible, device max size) | `DescriptorManager` | Vulkan heap buffers. Dominant RSS today (~100–256 MiB each). mimalloc will not shrink that. |
| Camera / object UBOs, DGC sequences | `ResourceManager` | persistently mapped device memory |
| Tracy GPU named pools | `tracyResourceAlloc` | tracks Vk/VMA pointers, not CRT |
| Stack / TLS / `.bss` | compiler | not heap |

After this work, Task Manager / VMMap **will still show ~230 MiB in Release** if descriptor heaps stay max-sized and mapped. mimalloc only changes the **CRT** slice (vectors, strings, image decode scratch, parser arenas). Measure that slice with `mi_stats_print` / `MIMALLOC_SHOW_STATS`, not with total RSS.

---

## 5. Files to change (when implementing)

CMake and one or two TUs. No store/parser rewrite.

| File | Change |
|---|---|
| `ThirdParty/CMakeLists.txt` | `MI_BUILD_SHARED ON`, `MI_BUILD_STATIC OFF`; alias shared `mimalloc`; keep `MI_USE_CXX`; `MI_WIN_REDIRECT OFF` until proven; Debug: `MI_TRACK_ASAN` genex (see §6) |
| `src/CMakeLists.txt` | copy `mimalloc` DLL/so next to exe; keep `ThirdParty::all` link |
| `src/main.cpp` | `mi_version()` (load) + replace smoke test with **STL + malloc** checks (`std::vector`, `new`, `malloc`). Drop `mi_stats_print` from every launch (too noisy); gate on env or a debug-only call |
| `src/pch.h` | keep `mimalloc.h` **or** drop it if no TU needs `mi_*` after main’s test. Do **not** add `mimalloc-new-delete.h` / `mimalloc-override.h` |
| one new TU e.g. `src/util/mi_new_delete.cpp` **only if** mimalloc DSO does not supply `new` to engine DLLs | `#include <mimalloc-new-delete.h>` compiled into each engine shared lib **or** only into a single DSO that every module links — prefer linking mimalloc’s own C++ overrides |
| `ThirdParty/implementations.cpp` | only if C malloc override misses stb/tinygltf: `STBI_*` / `TINYGLTF3_*` → `mi_*` |
| `src/CMakeLists.txt` / module CMake | `--wrap=malloc` **only** if §3.3 probe says so |

No changes to `geometry_store`, `material_store`, `vk_resource_manager`, shaders, or VMA.

---

## 6. Debug / ASan / Tracy

Current Debug: `-fsanitize=address,undefined` on engine **and** third-party (directory-scope). ASan replaces `malloc`. Two allocators on one process is undefined (false leaks, missed overflows, crashes).

| Config | mimalloc | ASan |
|---|---|---|
| Debug | build mimalloc with `MI_TRACK_ASAN=ON` (mimalloc poisons via ASan interface) **or** leave mimalloc override off in Debug and keep ASan as the allocator | keep engine `-fsanitize=address` |
| Release / RelWithDebInfo | full override, `MI_TRACK_ASAN=OFF` | sanitizers already off |

mimalloc docs: program must also be ASan-instrumented if mimalloc is `MI_TRACK_ASAN`. That matches current Debug flags.

If `MI_TRACK_ASAN` + `MI_OVERRIDE` fights ASan’s own `malloc` interceptor on this GNU Windows port, Debug fallback: `MI_OVERRIDE=OFF` for Debug only, engine keeps ASan, Release keeps mimalloc. Document which Debug path actually runs.

Tracy: CRT heap will not show up in current `TracyAllocN` GPU pools. Optional later: `MIMALLOC_SHOW_STATS` or a `mi_stats_*` plot. Not required for the replacement.

Guarded mode (`MI_GUARDED`) is ON in mimalloc’s own Debug by default. Fine for catching overflows; sample rate 0 until we want pages. Do not enable `MI_SECURE` unless asked (≈10% and extra VMAs).

---

## 7. Implementation order

Do not skip the probe steps. They decide wrap vs header vs “already done by shared + override”.

1. **CMake shared mimalloc**  
   Flip build knobs, alias, copy DLL. Confirm Release LTO still links (`MI_USE_CXX`).

2. **Symbol probe**  
   On `mimalloc.dll` and each `engine_*.dll`: who exports/imports `malloc`, `_Znwm`, `operator new`. Write the result into this doc when implementing (short table).

3. **Bind C++ new**  
   Until `new char[16]` from `engine_core` and from `main` both pass `mi_is_in_heap_region`. Add `mimalloc-new-delete.h` per DLL only if the probe shows libc++ still owns `_Znwm`.

4. **Bind C malloc**  
   Until `malloc(16)` from `ThirdPartyImpl` / `engine_core` passes `mi_is_in_heap_region`, **or** stb/tinygltf macros are set. Do not add `--wrap` if the DLL already exports `malloc`.

5. **Smoke in `main`**  
   Three pointers: `mi_malloc`, `malloc`, `std::vector<std::byte>(256).data()`. Assert all three in-heap (skip `malloc` assert only if §3.3 documented it as unreachable). No `mi_stats_print` on every run.

6. **Cross-DLL free**  
   Allocate `std::string` / `std::vector` in `engine_core`, destroy in `engine_runtime` (already happens for engine objects). ASan/Release run without heap mismatch.

7. **ASan Debug**  
   `MI_TRACK_ASAN` or Debug-off override. One Debug launch of room.obj without ASan reports on mimalloc internals.

---

## 8. Verification

```text
# Release, after DLL copy
MIMALLOC_VERBOSE=1 MIMALLOC_SHOW_STATS=1 ./engine.exe
```

Expect mimalloc banner, then stats with **binned/total ≫ 64 B**. If stats only show the smoke allocation, STL is still on libc++.

Code checks (debug assert or a one-shot in `main`):

```cpp
std::vector<std::byte> v(256);
assert(mi_is_in_heap_region(v.data()));

void* p = malloc(64);
assert(p == nullptr || mi_is_in_heap_region(p));
free(p);
```

`llvm-readobj --imports engine.exe` / `libengine_core.dll`: `mimalloc.dll` present; `_Znwm` not imported from `libc++.dll` once override is bound.

VMMap: CRT private heaps should shrink relative to “everything in mimalloc arenas”. Descriptor-heap mappings stay. Do not use total RSS as the pass/fail for this plan.

---

## 9. Risks

| Risk | Mitigation |
|---|---|
| Two heaps (static mimalloc in several DLLs) | shared mimalloc only |
| `new` in DLL A, `delete` in DLL B with different `operator delete` | same mimalloc DSO for every module; never define `mimalloc-new-delete.h` in only the exe |
| ASan + mimalloc double-intercept | §6; Debug may disable `MI_OVERRIDE` |
| ThinLTO duplicate `get_new_handler` | keep `MI_USE_CXX ON` |
| `--wrap=malloc` wrapping only our objects, not libc++ | acceptable; C++ path is `new`, not `malloc` |
| stb decode + `free` in another lib | if macros used, every free of that pointer must be `mi_free` |
| `mi_stats_print` on stdout every launch | remove from default `main`; env `MIMALLOC_SHOW_STATS` is enough |
| thinking RSS drop equals mimalloc working | heaps are mapped Vulkan; use mimalloc stats |

---

## 10. Out of scope unless asked later

- `mi_heap_t` per store (geometry scratch vs catalog) and bulk `mi_heap_destroy` on `clearScratch`
- `std::pmr` / `mi_stl_allocator` as the public container typedef
- Rebuilding SDL/fmt/spdlog/ktx against mimalloc
- Tracy plot of `mi_stats`
- Capping descriptor heaps (that is the large RSS; different plan)
