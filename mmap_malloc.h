#ifndef _MMAP_MALLOC_H
#define _MMAP_MALLOC_H 1

#ifdef __cplusplus
extern "C" {
#endif

#include <stdlib.h>

void* mmap_malloc(size_t size);
void* mmap_calloc(size_t count, size_t size);
void* mmap_realloc(void* ptr, size_t size);
void* mmap_reallocarray(void* ptr, size_t size, size_t count);
void mmap_free(void* ptr);

#ifdef __cplusplus
}
#endif

#endif /* mmap_malloc.h */
