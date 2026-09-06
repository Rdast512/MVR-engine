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

}
