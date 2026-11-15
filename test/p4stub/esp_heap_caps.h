/* Stand-in for ESP-IDF's esp_heap_caps.h on a PC: what lv_draw_ortho_ppa.c
 * takes its bands with. */
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define MALLOC_CAP_DMA (1 << 3)
#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_SPIRAM (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)
static inline void * heap_caps_aligned_calloc(size_t alignment, size_t n, size_t size, uint32_t caps)
{
    (void)caps;
    if(n == 0 || size > (SIZE_MAX - alignment) / n) return NULL;
    const size_t bytes = (n * size + alignment - 1) / alignment * alignment;
    void * p = aligned_alloc(alignment, bytes);
    if(p) memset(p, 0, bytes);
    return p;
}
static inline void heap_caps_free(void * p)
{
    free(p);
}
