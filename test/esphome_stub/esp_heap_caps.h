#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM (1 << 10)
static inline void *heap_caps_aligned_alloc(size_t a, size_t s, unsigned c) { (void) c; return aligned_alloc(a, s); }
static inline void heap_caps_free(void *p) { free(p); }
