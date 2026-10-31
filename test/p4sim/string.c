/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 youkorr
 * Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
 */

/* memcpy, memset and memmove a word at a time when they can, as ESP-IDF's
 * libc does, in place of picolibc's size-optimised ones, which go a byte at a
 * time and would make every copy on the simulated P4 four times dearer than
 * it is. */
#include <stddef.h>
#include <stdint.h>

void * memcpy(void * restrict dst, const void * restrict src, size_t n)
{
    uint8_t * d = dst;
    const uint8_t * s = src;
    if((((uintptr_t)d ^ (uintptr_t)s) & 3) == 0) {
        while(n && ((uintptr_t)d & 3)) { *d++ = *s++; n--; }
        uint32_t * dw = (uint32_t *)d;
        const uint32_t * sw = (const uint32_t *)s;
        while(n >= 16) {
            uint32_t a = sw[0], b = sw[1], c = sw[2], e = sw[3];
            dw[0] = a; dw[1] = b; dw[2] = c; dw[3] = e;
            dw += 4; sw += 4; n -= 16;
        }
        while(n >= 4) { *dw++ = *sw++; n -= 4; }
        d = (uint8_t *)dw;
        s = (const uint8_t *)sw;
    }
    while(n--) *d++ = *s++;
    return dst;
}

void * memmove(void * dst, const void * src, size_t n)
{
    uint8_t * d = dst;
    const uint8_t * s = src;
    if(d <= s || d >= s + n) return memcpy(dst, src, n);
    while(n--) d[n] = s[n];
    return dst;
}

void * memset(void * dst, int c, size_t n)
{
    uint8_t * d = dst;
    while(n && ((uintptr_t)d & 3)) { *d++ = (uint8_t)c; n--; }
    uint32_t w = (uint8_t)c * 0x01010101u;
    uint32_t * dw = (uint32_t *)d;
    while(n >= 16) { dw[0] = w; dw[1] = w; dw[2] = w; dw[3] = w; dw += 4; n -= 16; }
    while(n >= 4) { *dw++ = w; n -= 4; }
    d = (uint8_t *)dw;
    while(n--) *d++ = (uint8_t)c;
    return dst;
}
