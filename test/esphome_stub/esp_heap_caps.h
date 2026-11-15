#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM (1 << 10)
static inline void *heap_caps_aligned_alloc(size_t a, size_t s, unsigned c) { (void) c; return aligned_alloc(a, s); }
static inline void heap_caps_free(void *p) { free(p); }
#define MALLOC_CAP_INTERNAL (1 << 11)
static inline size_t heap_caps_get_free_size(unsigned c) { (void) c; return 256 * 1024; }
