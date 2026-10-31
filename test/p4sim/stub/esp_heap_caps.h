/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 youkorr
 * Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
 */

/* ESP-IDF's heap_caps for the P4 simulation: internal RAM is a static
 * arena, which the linker puts with the other globals (internal); anything
 * else is malloc, which p4bench.c's sbrk takes from the PSRAM. */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_SPIRAM (1 << 10)
#define MALLOC_CAP_8BIT (1 << 2)

static inline void * heap_caps_malloc(size_t size, uint32_t caps)
{
    if(caps & MALLOC_CAP_INTERNAL) {
        static uint8_t arena[512 * 1024] __attribute__((aligned(64)));
        static size_t used;
        size = (size + 63) & ~(size_t)63;
        if(used + size > sizeof(arena)) return NULL;
        void * p = arena + used;
        used += size;
        return p;
    }
    return malloc(size);
}

static inline void heap_caps_free(void * p)
{
    if((uintptr_t)p >= 0x90000000u) free(p);   /* the arena is never given back */
}
