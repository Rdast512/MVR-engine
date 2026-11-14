#include <mimalloc-new-delete.h>

#include <cstddef>
#include <mimalloc.h>

// gnu --wrap; one definition per dso
extern "C" {

void* __wrap_malloc(std::size_t size)
{
	return mi_malloc(size);
}

void* __wrap_calloc(std::size_t count, std::size_t size)
{
	return mi_calloc(count, size);
}

void* __wrap_realloc(void* p, std::size_t size)
{
	return mi_realloc(p, size);
}

void __wrap_free(void* p)
{
	mi_free(p);
}

void* __wrap_aligned_alloc(std::size_t alignment, std::size_t size)
{
	return mi_aligned_alloc(alignment, size);
}

// lld redirects a wrapped __imp_malloc (dllimport callers: MSVC-built static libs such as NGX) to __imp___wrap_malloc
// when it exists; its fallback local import is left unresolved (call through a zero displacement)
extern void* (*const wrapMallocImport)(std::size_t) asm("__imp___wrap_malloc");
extern void* (*const wrapCallocImport)(std::size_t, std::size_t) asm("__imp___wrap_calloc");
extern void* (*const wrapReallocImport)(void*, std::size_t) asm("__imp___wrap_realloc");
extern void (*const wrapFreeImport)(void*) asm("__imp___wrap_free");
extern void* (*const wrapAlignedAllocImport)(std::size_t, std::size_t) asm("__imp___wrap_aligned_alloc");
void* (*const wrapMallocImport)(std::size_t) = __wrap_malloc;
void* (*const wrapCallocImport)(std::size_t, std::size_t) = __wrap_calloc;
void* (*const wrapReallocImport)(void*, std::size_t) = __wrap_realloc;
void (*const wrapFreeImport)(void*) = __wrap_free;
void* (*const wrapAlignedAllocImport)(std::size_t, std::size_t) = __wrap_aligned_alloc;

}
