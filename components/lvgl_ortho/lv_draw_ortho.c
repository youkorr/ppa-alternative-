/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 youkorr
 * Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
 */

/**
 * @file lv_draw_ortho.c
 *
 * See lv_draw_ortho.h for what this is for. The comments here are about how.
 */

#include "lv_draw_ortho.h"

#if LV_USE_DRAW_ORTHO

#include "lvgl_private.h"
#include <math.h>
#include <string.h>

/* LVGL 10 renamed the draw task types after their descriptors. */
#if LVGL_VERSION_MAJOR < 10
#define LV_DRAW_DSC_TYPE_FILL           LV_DRAW_TASK_TYPE_FILL
#define LV_DRAW_DSC_TYPE_BORDER         LV_DRAW_TASK_TYPE_BORDER
#define LV_DRAW_DSC_TYPE_BOX_SHADOW     LV_DRAW_TASK_TYPE_BOX_SHADOW
#define LV_DRAW_DSC_TYPE_LETTER         LV_DRAW_TASK_TYPE_LETTER
#define LV_DRAW_DSC_TYPE_LABEL          LV_DRAW_TASK_TYPE_LABEL
#define LV_DRAW_DSC_TYPE_IMAGE          LV_DRAW_TASK_TYPE_IMAGE
#define LV_DRAW_DSC_TYPE_ARC            LV_DRAW_TASK_TYPE_ARC
#define LV_DRAW_DSC_TYPE_LINE           LV_DRAW_TASK_TYPE_LINE
#define LV_DRAW_DSC_TYPE_BLUR           LV_DRAW_TASK_TYPE_BLUR
#define LV_DRAW_DSC_TYPE_TRIANGLE       LV_DRAW_TASK_TYPE_TRIANGLE
#define LV_DRAW_DSC_TYPE_LAYER          LV_DRAW_TASK_TYPE_LAYER
#define LV_DRAW_DSC_TYPE_MASK_RECTANGLE LV_DRAW_TASK_TYPE_MASK_RECTANGLE
#define LV_DRAW_DSC_TYPE_VECTOR         LV_DRAW_TASK_TYPE_VECTOR
#endif

/* Any id no other unit uses. LVGL's own units count up from 1. */
#define DRAW_UNIT_ID_ORTHO 79

/**
 * A quarter-turn matrix reduced to integers, pixel for pixel:
 *     native_x = a * x + b * y + tx
 *     native_y = c * x + d * y + ty
 * The matrix LVGL hands over maps pixel EDGES (a vector renderer's view), so
 * a pixel's centre x + 0.5 lands on native_x + 0.5. Where a row of the matrix
 * sums to -1 that moves the integer result down by one; `ortho_from_matrix`
 * folds that in so nothing downstream has to know.
 */
typedef struct {
    int32_t a, b, c, d;
    int32_t tx, ty;
} ortho_t;

typedef struct _job_t job_t;

typedef struct {
    lv_draw_unit_t base;
    lv_draw_task_t * task_act;
    job_t * jobs;            /* LV_DRAW_ORTHO_MAX_BATCH */
    /* LV_DRAW_ORTHO_TILE_BYTES each, aligned to a cache line. The second only
     * with a turner: one is drawn while the other is being turned. */
    uint8_t * tile[2];
    void * tile_alloc[2];
    uint32_t ticket[2];      /* the last turn reading each tile, 0 if none */
    int32_t next;
} ortho_unit_t;

static ortho_unit_t * g_unit;
static lv_draw_ortho_stats_t g_stats;
static lv_draw_ortho_copy_cb_t g_copy_cb;
static bool g_fast = true;
static bool g_batch = true;
static bool g_tile_internal;

/* A turner (the PPA) and the last ticket it was handed that may still be
 * running, 0 when none is. */
static lv_draw_ortho_turn_cb_t g_turn;
static lv_draw_ortho_wait_cb_t g_wait;
static uint32_t g_last;

/* A turner writes by DMA, and its output has to start on a cache line: the
 * ESP32-P4's PPA wants its L2's, which is at most 128 bytes. */
#define TILE_ALIGN 128

/* The video window: a native rectangle of one display's frame buffer that
 * LVGL leaves to a source writing there itself. */
static lv_display_t * g_win_disp;
static lv_area_t g_win;

/* ------------------------------------------------------------------------ */
/* The matrix                                                               */
/* ------------------------------------------------------------------------ */

static bool near_int(float v, int32_t * out)
{
    float r = roundf(v);
    if(fabsf(v - r) > 1e-3f) return false;
    *out = (int32_t)r;
    return true;
}

static bool ortho_from_matrix(const lv_matrix_t * m, ortho_t * o)
{
    int32_t tx, ty;
    if(!near_int(m->m[0][0], &o->a) || !near_int(m->m[0][1], &o->b) ||
       !near_int(m->m[1][0], &o->c) || !near_int(m->m[1][1], &o->d) ||
       !near_int(m->m[0][2], &tx) || !near_int(m->m[1][2], &ty)) return false;

    /* No perspective. */
    if(fabsf(m->m[2][0]) > 1e-3f || fabsf(m->m[2][1]) > 1e-3f || fabsf(m->m[2][2] - 1.0f) > 1e-3f) return false;

    /* A signed permutation: exactly one +-1 per row and per column. */
    if(LV_ABS(o->a) + LV_ABS(o->b) != 1 || LV_ABS(o->c) + LV_ABS(o->d) != 1) return false;
    if(LV_ABS(o->a) + LV_ABS(o->c) != 1 || LV_ABS(o->b) + LV_ABS(o->d) != 1) return false;

    o->tx = tx + ((o->a + o->b) == 1 ? 0 : -1);
    o->ty = ty + ((o->c + o->d) == 1 ? 0 : -1);
    return true;
}

static inline bool ortho_is_identity(const ortho_t * o)
{
    return o->a == 1 && o->b == 0 && o->c == 0 && o->d == 1 && o->tx == 0 && o->ty == 0;
}

static inline void ortho_point(const ortho_t * o, int32_t x, int32_t y, int32_t * nx, int32_t * ny)
{
    *nx = o->a * x + o->b * y + o->tx;
    *ny = o->c * x + o->d * y + o->ty;
}

/* The inverse of a signed permutation is its transpose. */
static inline void ortho_unpoint(const ortho_t * o, int32_t nx, int32_t ny, int32_t * x, int32_t * y)
{
    int32_t u = nx - o->tx, v = ny - o->ty;
    *x = o->a * u + o->c * v;
    *y = o->b * u + o->d * v;
}

static void ortho_area(const ortho_t * o, const lv_area_t * in, lv_area_t * out)
{
    int32_t x1, y1, x2, y2;
    ortho_point(o, in->x1, in->y1, &x1, &y1);
    ortho_point(o, in->x2, in->y2, &x2, &y2);
    out->x1 = LV_MIN(x1, x2);
    out->x2 = LV_MAX(x1, x2);
    out->y1 = LV_MIN(y1, y2);
    out->y2 = LV_MAX(y1, y2);
}

static void ortho_unarea(const ortho_t * o, const lv_area_t * in, lv_area_t * out)
{
    int32_t x1, y1, x2, y2;
    ortho_unpoint(o, in->x1, in->y1, &x1, &y1);
    ortho_unpoint(o, in->x2, in->y2, &x2, &y2);
    out->x1 = LV_MIN(x1, x2);
    out->x2 = LV_MAX(x1, x2);
    out->y1 = LV_MIN(y1, y2);
    out->y2 = LV_MAX(y1, y2);
}

/* ------------------------------------------------------------------------ */
/* The video window                                                         */
/* ------------------------------------------------------------------------ */

/* Whether writes into this layer must keep out of the window: only the
 * display's own layer, rendered straight into its frame buffer. A child
 * layer is a separate buffer, and in partial mode the band is copied by the
 * flush callback, which is where a window would have to be honoured. */
static bool has_window(const lv_layer_t * layer)
{
    if(g_win_disp == NULL || layer->parent != NULL) return false;
    if(lv_refr_get_disp_refreshing() != g_win_disp) return false;
    return lv_display_get_render_mode(g_win_disp) == LV_DISPLAY_RENDER_MODE_DIRECT;
}

/* The parts of a native rectangle outside the window: up to four. */
static int32_t outside_window(const lv_layer_t * layer, const lv_area_t * nat, lv_area_t out[4])
{
    lv_area_t w;
    if(!has_window(layer) || !lv_area_intersect(&w, nat, &g_win)) {
        out[0] = *nat;
        return 1;
    }
    int32_t n = 0;
    if(nat->y1 < w.y1) lv_area_set(&out[n++], nat->x1, nat->y1, nat->x2, w.y1 - 1);
    if(nat->y2 > w.y2) lv_area_set(&out[n++], nat->x1, w.y2 + 1, nat->x2, nat->y2);
    if(nat->x1 < w.x1) lv_area_set(&out[n++], nat->x1, w.y1, w.x1 - 1, w.y2);
    if(nat->x2 > w.x2) lv_area_set(&out[n++], w.x2 + 1, w.y1, nat->x2, w.y2);
    g_stats.window_skips++;
    return n;
}

/* ------------------------------------------------------------------------ */
/* Moving pixels                                                            */
/* ------------------------------------------------------------------------ */

static void copy_straight(uint8_t * dest, int32_t dest_stride, const uint8_t * src, int32_t src_stride,
                          int32_t row_bytes, int32_t h)
{
    g_stats.bytes_straight += (uint64_t)row_bytes * (uint64_t)h;
    if(g_copy_cb && g_copy_cb(dest, dest_stride, src, src_stride, row_bytes, h)) return;
    for(int32_t r = 0; r < h; r++) {
        lv_memcpy(dest, src, row_bytes);
        dest += dest_stride;
        src += src_stride;
    }
}

/* Square blocks a transpose works in. A native row walks a column of the
 * upright side, touching one cache line per pixel; done a block at a time,
 * the BLOCK lines a block touches are still cached when the next native row
 * of the same block wants the pixel beside the one it just read. */
#ifndef LV_DRAW_ORTHO_BLOCK
#define LV_DRAW_ORTHO_BLOCK 32
#endif

/**
 * Move the pixels of a NATIVE rectangle between the native buffer and an
 * upright one, in either direction. `up` addresses upright pixel (x, y) as
 * up + (y - up_area->y1) * up_stride + (x - up_area->x1) * px.
 */
static void move_turned(const ortho_t * o, int32_t px,
                        uint8_t * nat, int32_t nat_stride, const lv_area_t * nat_buf_area,
                        uint8_t * up, int32_t up_stride, const lv_area_t * up_area,
                        const lv_area_t * nat_rect, bool to_native)
{
    /* One step right in native space is (a, b) in upright space. */
    const intptr_t up_step = (intptr_t)o->a * px + (intptr_t)o->b * up_stride;
    g_stats.bytes_turned += (uint64_t)lv_area_get_width(nat_rect) *
                            (uint64_t)lv_area_get_height(nat_rect) * (uint64_t)px;

    /* Two native rows next to each other are two upright columns next to each
     * other, so in 16 bits one 32-bit read of the upright side carries a pixel
     * of each, and two such reads a column apart fill a 32-bit write of each
     * native row: a quarter of the loads and stores of a pixel at a time. A
     * P4 core is in order and has no wider load, so this is where its turn
     * spends its time. */
    const int32_t col_step = o->c * px + o->d * up_stride;  /* one native row down, upright */
    const bool quad = px == 2 && (col_step == 2 || col_step == -2) &&
                      (up_step == up_stride || up_step == -up_stride);

    for(int32_t by = nat_rect->y1; by <= nat_rect->y2; by += LV_DRAW_ORTHO_BLOCK) {
        const int32_t ey = LV_MIN(by + LV_DRAW_ORTHO_BLOCK - 1, nat_rect->y2);
        for(int32_t bx = nat_rect->x1; bx <= nat_rect->x2; bx += LV_DRAW_ORTHO_BLOCK) {
            const int32_t w = LV_MIN(bx + LV_DRAW_ORTHO_BLOCK - 1, nat_rect->x2) - bx + 1;
            int32_t ny = by;
            if(quad) {
                for(; ny + 1 <= ey; ny += 2) {
                    int32_t x, y;
                    ortho_unpoint(o, bx, ny, &x, &y);
                    uint8_t * u = up + (intptr_t)(y - up_area->y1) * up_stride + (intptr_t)(x - up_area->x1) * px;
                    uint16_t * n0 = (uint16_t *)(nat + (intptr_t)(ny - nat_buf_area->y1) * nat_stride +
                                                 (intptr_t)(bx - nat_buf_area->x1) * px);
                    uint16_t * n1 = (uint16_t *)((uint8_t *)n0 + nat_stride);
                    /* The pair in upright memory: row ny at the lower address
                     * when the next native row is one pixel right upright.
                     * Halves of a 32-bit word in little-endian order, which
                     * the P4 and every host this is tested on are. */
                    uint8_t * pair = col_step > 0 ? u : u - 2;
                    int32_t i = 0;
                    if(((uintptr_t)pair & 3) == 0 && ((uintptr_t)n0 & 3) == 0 && (nat_stride & 3) == 0 &&
                       (up_stride & 3) == 0) {
                        const intptr_t two = 2 * up_step;
                        for(; i + 1 < w; i += 2, pair += two) {
                            uint32_t * a = (uint32_t *)pair;
                            uint32_t * b = (uint32_t *)(pair + up_step);
                            uint32_t * w0 = (uint32_t *)(n0 + i);
                            uint32_t * w1 = (uint32_t *)(n1 + i);
                            if(to_native) {
                                const uint32_t va = *a, vb = *b;
                                if(col_step > 0) {
                                    *w0 = (va & 0xffffu) | (vb << 16);
                                    *w1 = (va >> 16) | (vb & 0xffff0000u);
                                }
                                else {
                                    *w0 = (va >> 16) | (vb & 0xffff0000u);
                                    *w1 = (va & 0xffffu) | (vb << 16);
                                }
                            }
                            else {
                                const uint32_t v0 = *w0, v1 = *w1;
                                if(col_step > 0) {
                                    *a = (v0 & 0xffffu) | (v1 << 16);
                                    *b = (v0 >> 16) | (v1 & 0xffff0000u);
                                }
                                else {
                                    *a = (v1 & 0xffffu) | (v0 << 16);
                                    *b = (v1 >> 16) | (v0 & 0xffff0000u);
                                }
                            }
                        }
                    }
                    /* What is left, and any pair that is not word-aligned. */
                    uint8_t * u0 = u + (intptr_t)i * up_step;
                    for(; i < w; i++, u0 += up_step) {
                        uint16_t * p0 = (uint16_t *)u0;
                        uint16_t * p1 = (uint16_t *)(u0 + col_step);
                        if(to_native) {
                            n0[i] = *p0;
                            n1[i] = *p1;
                        }
                        else {
                            *p0 = n0[i];
                            *p1 = n1[i];
                        }
                    }
                }
            }
            for(; ny <= ey; ny++) {
                int32_t x, y;
                ortho_unpoint(o, bx, ny, &x, &y);
                uint8_t * u = up + (intptr_t)(y - up_area->y1) * up_stride + (intptr_t)(x - up_area->x1) * px;
                uint8_t * n = nat + (intptr_t)(ny - nat_buf_area->y1) * nat_stride +
                              (intptr_t)(bx - nat_buf_area->x1) * px;
                if(px == 2) {
                    uint16_t * n16 = (uint16_t *)n;
                    if(to_native) for(int32_t i = 0; i < w; i++, u += up_step) n16[i] = *(const uint16_t *)u;
                    else for(int32_t i = 0; i < w; i++, u += up_step) *(uint16_t *)u = n16[i];
                }
                else if(px == 4) {
                    uint32_t * n32 = (uint32_t *)n;
                    if(to_native) for(int32_t i = 0; i < w; i++, u += up_step) n32[i] = *(const uint32_t *)u;
                    else for(int32_t i = 0; i < w; i++, u += up_step) *(uint32_t *)u = n32[i];
                }
                else {
                    for(int32_t i = 0; i < w; i++, u += up_step, n += px) {
                        if(to_native) lv_memcpy(n, u, px);
                        else lv_memcpy(u, n, px);
                    }
                }
            }
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Turning with a turner                                                    */
/* ------------------------------------------------------------------------ */

/* Wait for everything handed to the turner. Whatever the CPU is about to read
 * or write in a buffer a queued turn may still be reading or writing, it does
 * after this -- and it is done at the end of every run of tasks, so nothing is
 * left running when LVGL takes its layer back. */
static void fence(void)
{
    if(g_last && g_wait) {
        g_wait(g_last);
        g_stats.waits++;
    }
    g_last = 0;
}

/* The display rotation a quarter-turn matrix is; false for a mirror. */
static bool ortho_rotation(const ortho_t * o, lv_display_rotation_t * rot)
{
    if(o->a == 1 && o->d == 1 && o->b == 0 && o->c == 0) *rot = LV_DISPLAY_ROTATION_0;
    else if(o->a == -1 && o->d == -1 && o->b == 0 && o->c == 0) *rot = LV_DISPLAY_ROTATION_180;
    else if(o->a == 0 && o->d == 0 && o->b == 1 && o->c == -1) *rot = LV_DISPLAY_ROTATION_90;
    else if(o->a == 0 && o->d == 0 && o->b == -1 && o->c == 1) *rot = LV_DISPLAY_ROTATION_270;
    else return false;
    return true;
}

static lv_display_rotation_t opposite(lv_display_rotation_t r)
{
    return r == LV_DISPLAY_ROTATION_90 ? LV_DISPLAY_ROTATION_270
           : r == LV_DISPLAY_ROTATION_270 ? LV_DISPLAY_ROTATION_90 : r;
}

/* move_turned's job, handed to the turner if there is one and it takes it;
 * else done by the CPU, after everything queued has finished. Returns the
 * turner's ticket, 0 when the CPU did it. */
static uint32_t turn_or_cpu(const ortho_t * o, lv_color_format_t cf, int32_t px,
                            uint8_t * nat, int32_t nat_stride, const lv_area_t * nat_buf_area,
                            uint8_t * up, int32_t up_stride, const lv_area_t * up_area,
                            const lv_area_t * nat_rect, bool to_native)
{
    lv_display_rotation_t rot;
    if(g_turn && ortho_rotation(o, &rot)) {
        lv_area_t up_rect, blk;
        ortho_unarea(o, nat_rect, &up_rect);
        const lv_draw_ortho_pic_t np = {nat, (uint32_t)nat_stride, (uint32_t)(nat_stride / px),
                                        (uint32_t)lv_area_get_height(nat_buf_area)
                                       };
        const lv_draw_ortho_pic_t upic = {up, (uint32_t)up_stride, (uint32_t)(up_stride / px),
                                          (uint32_t)lv_area_get_height(up_area)
                                         };
        uint32_t t;
        if(to_native) {
            blk = up_rect;
            lv_area_move(&blk, -up_area->x1, -up_area->y1);
            t = g_turn(&upic, &blk, &np, nat_rect->x1 - nat_buf_area->x1, nat_rect->y1 - nat_buf_area->y1, rot, cf);
        }
        else {
            blk = *nat_rect;
            lv_area_move(&blk, -nat_buf_area->x1, -nat_buf_area->y1);
            t = g_turn(&np, &blk, &upic, up_rect.x1 - up_area->x1, up_rect.y1 - up_area->y1, opposite(rot), cf);
        }
        if(t) {
            const uint64_t bytes = (uint64_t)lv_area_get_size(nat_rect) * (uint64_t)px;
            g_stats.bytes_turned += bytes;
            g_stats.bytes_handed += bytes;
            g_last = t;
            return t;
        }
    }
    fence();
    move_turned(o, px, nat, nat_stride, nat_buf_area, up, up_stride, up_area, nat_rect, to_native);
    return 0;
}

static void wait_ticket(uint32_t * t)
{
    if(*t && g_wait) {
        g_wait(*t);
        g_stats.waits++;
    }
    *t = 0;
}

/* ------------------------------------------------------------------------ */
/* The software renderer, run on a copy of the task                         */
/* ------------------------------------------------------------------------ */

#if LVGL_VERSION_MAJOR >= 10
/* LVGL 10 draws a descriptor onto a layer within a clip area: no task. */
static void sw_execute_on(const lv_draw_task_t * t, lv_layer_t * layer, const lv_area_t * area,
                          const lv_area_t * clip)
{
    void * d = t->draw_dsc;
    switch(t->type) {
        case LV_DRAW_DSC_TYPE_FILL:
            lv_draw_sw_fill(layer, clip, d, area);
            break;
        case LV_DRAW_DSC_TYPE_BORDER:
            lv_draw_sw_border(layer, clip, d, area);
            break;
        case LV_DRAW_DSC_TYPE_BOX_SHADOW:
            lv_draw_sw_box_shadow(layer, clip, d, area);
            break;
        case LV_DRAW_DSC_TYPE_LETTER:
            lv_draw_sw_letter(layer, clip, d, area);
            break;
        case LV_DRAW_DSC_TYPE_LABEL:
            lv_draw_sw_label(layer, clip, d, area);
            break;
        case LV_DRAW_DSC_TYPE_IMAGE:
            lv_draw_sw_image(layer, clip, d, area);
            break;
        case LV_DRAW_DSC_TYPE_ARC:
            lv_draw_sw_arc(layer, clip, d, area);
            break;
        case LV_DRAW_DSC_TYPE_LINE:
            lv_draw_line_iterate(layer, clip, d, lv_draw_sw_line, NULL);
            break;
        case LV_DRAW_DSC_TYPE_BLUR:
            lv_draw_sw_blur(layer, clip, d, area);
            break;
        case LV_DRAW_DSC_TYPE_TRIANGLE:
            lv_draw_sw_triangle(layer, clip, d);
            break;
        case LV_DRAW_DSC_TYPE_LAYER:
            lv_draw_sw_layer(layer, clip, d, area);
            break;
        case LV_DRAW_DSC_TYPE_MASK_RECTANGLE:
            lv_draw_sw_mask_rect(layer, clip, d);
            break;
#if LV_USE_VECTOR_GRAPHIC && LV_USE_THORVG
        case LV_DRAW_DSC_TYPE_VECTOR:
            lv_draw_sw_vector(layer, clip, d);
            break;
#endif
        default:
            break;
    }
}
#else
/* LVGL 9 draws a task: a copy of it, pointed at the layer and the clip. */
static void sw_execute(lv_draw_task_t * t)
{
    switch(t->type) {
        case LV_DRAW_TASK_TYPE_FILL:
            lv_draw_sw_fill(t, t->draw_dsc, &t->area);
            break;
        case LV_DRAW_TASK_TYPE_BORDER:
            lv_draw_sw_border(t, t->draw_dsc, &t->area);
            break;
        case LV_DRAW_TASK_TYPE_BOX_SHADOW:
            lv_draw_sw_box_shadow(t, t->draw_dsc, &t->area);
            break;
        case LV_DRAW_TASK_TYPE_LETTER:
            lv_draw_sw_letter(t, t->draw_dsc, &t->area);
            break;
        case LV_DRAW_TASK_TYPE_LABEL:
            lv_draw_sw_label(t, t->draw_dsc, &t->area);
            break;
        case LV_DRAW_TASK_TYPE_IMAGE:
            lv_draw_sw_image(t, t->draw_dsc, &t->area);
            break;
        case LV_DRAW_TASK_TYPE_ARC:
            lv_draw_sw_arc(t, t->draw_dsc, &t->area);
            break;
        case LV_DRAW_TASK_TYPE_LINE:
            lv_draw_line_iterate(t, t->draw_dsc, lv_draw_sw_line);
            break;
        case LV_DRAW_TASK_TYPE_BLUR:
            lv_draw_sw_blur(t, t->draw_dsc, &t->area);
            break;
        case LV_DRAW_TASK_TYPE_TRIANGLE:
            lv_draw_sw_triangle(t, t->draw_dsc);
            break;
        case LV_DRAW_TASK_TYPE_LAYER:
            lv_draw_sw_layer(t, t->draw_dsc, &t->area);
            break;
        case LV_DRAW_TASK_TYPE_MASK_RECTANGLE:
            lv_draw_sw_mask_rect(t, t->draw_dsc);
            break;
#if LV_USE_VECTOR_GRAPHIC && LV_USE_THORVG
        case LV_DRAW_TASK_TYPE_VECTOR:
            lv_draw_sw_vector(t, t->draw_dsc);
            break;
#endif
        default:
            break;
    }
}

static void sw_execute_on(const lv_draw_task_t * t, lv_layer_t * layer, const lv_area_t * area,
                          const lv_area_t * clip)
{
    lv_draw_task_t copy = *t;
    copy.next = NULL;
    copy.target_layer = layer;
    copy.area = *area;
    copy.clip_area = *clip;
    lv_matrix_identity(&copy.matrix);
    sw_execute(&copy);
}
#endif

static void tile_layer(lv_layer_t * tl, lv_draw_buf_t * tb, const lv_area_t * area,
                       const lv_layer_t * target)
{
    lv_memzero(tl, sizeof(*tl));
    tl->draw_buf = tb;
    tl->buf_area = *area;
    tl->phy_clip_area = *area;
    tl->_clip_area = *area;
    tl->color_format = target->color_format;
    tl->opa = LV_OPA_COVER;
    tl->recolor = target->recolor;
    lv_matrix_identity(&tl->matrix);
}

/* ------------------------------------------------------------------------ */
/* The general path: draw upright into a tile, put it back turned           */
/* ------------------------------------------------------------------------ */

/* Up to this many consecutive tasks are drawn through one set of tiles, so a
 * screen's worth of widgets costs one turn rather than one per widget. */
#ifndef LV_DRAW_ORTHO_MAX_BATCH
#define LV_DRAW_ORTHO_MAX_BATCH 64
#endif

struct _job_t {
    lv_draw_task_t * t;
    lv_area_t todo;   /* upright: what the task may change */
    lv_area_t hole;   /* upright: inside `todo` but certainly not changed, or x2 < x1 */
};

/* Whether a fill paints every pixel of `tile` opaquely, so what was under the
 * tile never shows and need not be read back. */
static bool covers(const job_t * j, const lv_area_t * tile)
{
    const lv_draw_task_t * t = j->t;
    if(t->type != LV_DRAW_DSC_TYPE_FILL || t->opa < LV_OPA_MAX) return false;
    const lv_draw_fill_dsc_t * d = t->draw_dsc;
    if(d->opa < LV_OPA_MAX) return false;
    if(d->grad.dir != LV_GRAD_DIR_NONE) {
        if(d->grad.dir != LV_GRAD_DIR_HOR && d->grad.dir != LV_GRAD_DIR_VER) return false;
        for(uint32_t i = 0; i < d->grad.stops_count; i++) if(d->grad.stops[i].opa < LV_OPA_MAX) return false;
    }
    if(!lv_area_is_in(tile, &t->clip_area, 0)) return false;
    if(!lv_area_is_in(tile, &t->area, 0)) return false;
    if(d->radius == 0) return true;
    /* Clear of the four corner squares is enough. */
    const int32_t r = LV_MIN(d->radius, LV_MIN(lv_area_get_width(&t->area), lv_area_get_height(&t->area)) / 2);
    lv_area_t a = t->area;
    a.x1 += r;
    a.x2 -= r;
    if(lv_area_is_in(tile, &a, 0)) return true;
    a = t->area;
    a.y1 += r;
    a.y2 -= r;
    return lv_area_is_in(tile, &a, 0);
}

/* The part of a task's area it is certain not to change. A border never
 * touches the middle of its rectangle; a shadow under an opaque background
 * is painted over by that background, which is the next task. Leaving these
 * out keeps a large panel from turning all its pixels for its outline. */
static void find_hole(job_t * j)
{
    const lv_draw_task_t * t = j->t;
    lv_area_t h = {0, 0, -1, -1};
    if(t->type == LV_DRAW_DSC_TYPE_BORDER) {
        const lv_draw_border_dsc_t * d = t->draw_dsc;
        int32_t in = LV_MAX(d->width, d->radius);
        h = t->area;
        lv_area_increase(&h, -in, -in);
    }
    else if(t->type == LV_DRAW_DSC_TYPE_BOX_SHADOW) {
        const lv_draw_box_shadow_dsc_t * d = t->draw_dsc;
        if(d->bg_cover && t->next && t->next->type == LV_DRAW_DSC_TYPE_FILL) {
            const lv_draw_fill_dsc_t * f = t->next->draw_dsc;
            if(f->opa >= LV_OPA_MAX && t->next->opa >= LV_OPA_MAX &&
               lv_area_is_in(&t->area, &t->next->area, 0) && lv_area_is_in(&t->next->area, &t->area, 0)) {
                h = t->area;
                lv_area_increase(&h, -d->radius, -d->radius);
            }
        }
    }
    if(lv_area_get_width(&h) <= 0 || lv_area_get_height(&h) <= 0) {
        lv_area_set(&j->hole, 0, 0, -1, -1);
        return;
    }
    j->hole = h;
}

static bool job_touches(const job_t * j, const lv_area_t * tile)
{
    lv_area_t a;
    if(!lv_area_intersect(&a, &j->todo, tile)) return false;
    /* Wholly inside the hole: nothing to do here. */
    if(j->hole.x2 >= j->hole.x1 && lv_area_is_in(&a, &j->hole, 0)) return false;
    return true;
}

static void draw_tile(const job_t * jobs, int32_t n, const ortho_t * o, const lv_area_t * whole_tile,
                      uint8_t * mem, size_t mem_size, uint32_t * ticket)
{
    /* Only the part of the tile the jobs reach: a run of a scroll bar down
     * the right edge and another along the bottom spans the whole screen, and
     * turning the empty middle of it is all cost and no picture. */
    int32_t first = -1;
    lv_area_t reach = {0, 0, -1, -1};
    for(int32_t i = 0; i < n; i++) {
        lv_area_t a;
        if(!job_touches(&jobs[i], whole_tile)) continue;
        lv_area_intersect(&a, &jobs[i].todo, whole_tile);
        if(first < 0) {
            first = i;
            reach = a;
        }
        else lv_area_join(&reach, &reach, &a);
    }
    if(first < 0) return;
    const lv_area_t * tile = &reach;

    lv_layer_t * target = jobs[first].t->target_layer;
    lv_draw_buf_t * nb = target->draw_buf;
    const lv_color_format_t cf = nb->header.cf;
    const int32_t px = lv_color_format_get_size(cf);
    const int32_t w = lv_area_get_width(tile);
    const int32_t h = lv_area_get_height(tile);
    const uint32_t stride = lv_draw_buf_width_to_stride(w, cf);

    lv_draw_buf_t tb;
    if(lv_draw_buf_init(&tb, w, h, cf, stride, mem, mem_size) != LV_RESULT_OK) {
        LV_LOG_WARN("ortho: tile %dx%d does not fit", (int)w, (int)h);
        return;
    }

    lv_area_t nat;
    ortho_area(o, tile, &nat);

    /* The tile starts as what is already under it, so blending is right --
     * unless the first thing drawn paints all of it anyway. */
    if(!covers(&jobs[first], tile)) {
        uint32_t t = turn_or_cpu(o, cf, px, nb->data, nb->header.stride, &target->buf_area,
                                 tb.data, stride, tile, &nat, false);
        wait_ticket(&t);   /* it is drawn on next */
    }
    else g_stats.tiles_unread++;

    lv_layer_t tl;
    tile_layer(&tl, &tb, tile, target);
    for(int32_t i = first; i < n; i++) {
        lv_area_t clip, drawn;
        if(!job_touches(&jobs[i], tile)) continue;
        if(lv_area_intersect(&clip, &jobs[i].t->clip_area, tile)) {
            sw_execute_on(jobs[i].t, &tl, &jobs[i].t->area, &clip);
            if(lv_area_intersect(&drawn, &jobs[i].todo, &clip))
                g_stats.bytes_drawn += (uint64_t)lv_area_get_size(&drawn) * (uint64_t)px;
        }
    }

    lv_area_t parts[4];
    const int32_t np = outside_window(target, &nat, parts);
    for(int32_t i = 0; i < np; i++) {
        const uint32_t t = turn_or_cpu(o, cf, px, nb->data, nb->header.stride, &target->buf_area,
                                       tb.data, stride, tile, &parts[i], true);
        if(t) *ticket = t;
    }
    g_stats.tiles++;
}

static void draw_jobs(ortho_unit_t * u, const job_t * jobs, int32_t n, const ortho_t * o)
{
    const lv_color_format_t cf = jobs[0].t->target_layer->draw_buf->header.cf;
    lv_area_t box = jobs[0].todo;
    for(int32_t i = 1; i < n; i++) lv_area_join(&box, &box, &jobs[i].todo);
    g_stats.tasks_general += n;

    /* A blur reads its neighbours, so it is never cut into tiles. */
    if(n == 1 && jobs[0].t->type == LV_DRAW_DSC_TYPE_BLUR) {
        const uint32_t stride = lv_draw_buf_width_to_stride(lv_area_get_width(&box), cf);
        size_t size = (size_t)stride * lv_area_get_height(&box) + LV_DRAW_BUF_ALIGN;
        void * raw = lv_malloc(size);
        if(raw == NULL) {
            LV_LOG_WARN("ortho: no memory for a blur");
            return;
        }
        uint32_t t = 0;
        draw_tile(jobs, 1, o, &box, lv_draw_buf_align(raw, cf), size - LV_DRAW_BUF_ALIGN, &t);
        fence();   /* a turner may still be reading it */
        lv_free(raw);
        return;
    }

    /* As wide as the area allows, then as many rows as fit. */
    const int32_t w = lv_area_get_width(&box);
    int32_t tw = w;
    while(tw > 1 && lv_draw_buf_width_to_stride(tw, cf) > LV_DRAW_ORTHO_TILE_BYTES) tw = (tw + 1) / 2;
    const uint32_t tstride = lv_draw_buf_width_to_stride(tw, cf);
    int32_t th = (int32_t)(LV_DRAW_ORTHO_TILE_BYTES / tstride);
    if(th < 1) th = 1;

    for(int32_t y = box.y1; y <= box.y2; y += th) {
        for(int32_t x = box.x1; x <= box.x2; x += tw) {
            lv_area_t tile = {x, y, LV_MIN(x + tw - 1, box.x2), LV_MIN(y + th - 1, box.y2)};
            /* With two tiles, this one is drawn while the turner still has
             * the other: it waits only for the turns reading THIS one. */
            const int32_t k = u->next;
            if(u->tile[1]) u->next ^= 1;
            wait_ticket(&u->ticket[k]);
            draw_tile(jobs, n, o, &tile, u->tile[k], LV_DRAW_ORTHO_TILE_BYTES, &u->ticket[k]);
        }
    }
    fence();
    u->ticket[0] = u->ticket[1] = 0;
}

/* ------------------------------------------------------------------------ */
/* Fast paths                                                               */
/* ------------------------------------------------------------------------ */

/* A solid fill turned is the fill of the turned rectangle: a corner radius is
 * the same on every side, so nothing about it changes. */
static bool try_fill(lv_draw_task_t * t, const ortho_t * o, const lv_area_t * todo)
{
    if(t->type != LV_DRAW_DSC_TYPE_FILL) return false;
    lv_draw_fill_dsc_t * dsc = t->draw_dsc;
    if(dsc->grad.dir != LV_GRAD_DIR_NONE) return false;

    lv_area_t area, clip, parts[4];
    ortho_area(o, &t->area, &area);
    ortho_area(o, todo, &clip);
    const int32_t np = outside_window(t->target_layer, &clip, parts);
    for(int32_t i = 0; i < np; i++) sw_execute_on(t, t->target_layer, &area, &parts[i]);
    g_stats.fills_direct++;
    return true;
}

static bool plain_image(const lv_draw_task_t * t, const lv_image_dsc_t ** src_out)
{
    if(t->type != LV_DRAW_DSC_TYPE_IMAGE) return false;
    const lv_draw_image_dsc_t * d = t->draw_dsc;
    if(lv_image_src_get_type(d->src) != LV_IMAGE_SRC_VARIABLE) return false;
    if(d->rotation != 0 || d->scale_x != LV_SCALE_NONE || d->scale_y != LV_SCALE_NONE) return false;
    if(d->skew_x != 0 || d->skew_y != 0 || d->tile || d->clip_radius != 0) return false;
    if(d->recolor_opa > LV_OPA_MIN || d->opa < LV_OPA_MAX || t->opa < LV_OPA_MAX) return false;
    if(d->colorkey || d->bitmap_mask_src || d->blend_mode != LV_BLEND_MODE_NORMAL) return false;

    const lv_image_dsc_t * img = d->src;
    const lv_color_format_t layer_cf = t->target_layer->draw_buf->header.cf;
    if(img->header.cf != layer_cf) return false;
    if(layer_cf != LV_COLOR_FORMAT_RGB565 && layer_cf != LV_COLOR_FORMAT_RGB888 &&
       layer_cf != LV_COLOR_FORMAT_XRGB8888) return false;
    if(lv_area_get_width(&t->area) != (int32_t)img->header.w ||
       lv_area_get_height(&t->area) != (int32_t)img->header.h) return false;
    if(img->data == NULL) return false;
    *src_out = img;
    return true;
}

static bool try_image(lv_draw_task_t * t, const ortho_t * o, const lv_area_t * todo)
{
    const lv_image_dsc_t * img;
    if(!plain_image(t, &img)) return false;

    lv_layer_t * target = t->target_layer;
    lv_draw_buf_t * nb = target->draw_buf;
    const int32_t px = lv_color_format_get_size(nb->header.cf);
    lv_area_t all, parts[4];
    ortho_area(o, todo, &all);
    const int32_t np = outside_window(target, &all, parts);

    if(img->header.flags & LV_DRAW_ORTHO_IMAGE_FLAG_NATIVE) {
        /* Already in the panel's orientation: its rows ARE native rows. */
        lv_area_t whole;
        ortho_area(o, &t->area, &whole);
        const int32_t sstride = img->header.stride ? (int32_t)img->header.stride
                                : lv_area_get_width(&whole) * px;
        for(int32_t i = 0; i < np; i++) {
            const lv_area_t * nat = &parts[i];
            const uint8_t * s = img->data + (intptr_t)(nat->y1 - whole.y1) * sstride +
                                (intptr_t)(nat->x1 - whole.x1) * px;
            uint8_t * dst = nb->data + (intptr_t)(nat->y1 - target->buf_area.y1) * nb->header.stride +
                            (intptr_t)(nat->x1 - target->buf_area.x1) * px;
            copy_straight(dst, nb->header.stride, s, sstride, lv_area_get_width(nat) * px,
                          lv_area_get_height(nat));
        }
        g_stats.images_native++;
        return true;
    }

    const int32_t sstride = img->header.stride ? (int32_t)img->header.stride
                            : (int32_t)img->header.w * px;
    for(int32_t i = 0; i < np; i++) {
        turn_or_cpu(o, nb->header.cf, px, nb->data, nb->header.stride, &target->buf_area,
                    (uint8_t *)img->data, sstride, &t->area, &parts[i], true);
    }
    fence();   /* the picture is the application's again when the task is finished */
    g_stats.images_turned++;
    return true;
}

/* ------------------------------------------------------------------------ */
/* The draw unit                                                            */
/* ------------------------------------------------------------------------ */

/* A label's area is the size of its text, and a glyph may still reach past
 * it -- a bearing, an accent, a letter's anti-aliased edge. The software
 * renderer is held back only by the clip area, and drawing tiles over the
 * whole clip area would turn every pixel of a screen for one word. A glyph
 * cannot reach further than a line height from the text it belongs to, so
 * that is the margin. */
static void widen_for_glyphs(const lv_draw_task_t * t, lv_area_t * area)
{
    const lv_font_t * font = NULL;
    if(t->type == LV_DRAW_DSC_TYPE_LABEL) font = ((const lv_draw_label_dsc_t *)t->draw_dsc)->font;
    else if(t->type == LV_DRAW_DSC_TYPE_LETTER) font = ((const lv_draw_letter_dsc_t *)t->draw_dsc)->font;
    else return;
    int32_t m = font ? lv_font_get_line_height(font) : 32;
    lv_area_increase(area, m, m);
}

/* What a task may change, upright, inside the buffer; false if nothing. */
static bool task_todo(const lv_draw_task_t * t, const ortho_t * o, lv_area_t * todo)
{
    lv_area_t buf_up, real = t->_real_area;
    widen_for_glyphs(t, &real);
    if(!lv_area_intersect(todo, &real, &t->clip_area)) return false;
    ortho_unarea(o, &t->target_layer->buf_area, &buf_up);
    return lv_area_intersect(todo, todo, &buf_up);
}

/* A task that goes faster on its own than in a batch.
 *
 * A plain image turned straight into the frame buffer saves its pixels one
 * upright copy -- but leaving the batch splits the run around it, and every
 * tile the second half of the run shares with the first is read back out of
 * the frame buffer and turned again. For an icon that costs far more than it
 * saves: a 97x61 picture in the middle of a UI split one run in two and
 * turned 16% of the screen twice. So an icon stays in the run, and only a
 * picture of LV_DRAW_ORTHO_ALONE_BYTES or more (a camera frame, a wallpaper)
 * goes alone -- and always one stored native, which only the fast path can
 * draw. */
#ifndef LV_DRAW_ORTHO_ALONE_BYTES
#define LV_DRAW_ORTHO_ALONE_BYTES (32 * 1024)
#endif
static bool fast_alone(const lv_draw_task_t * t)
{
    const lv_image_dsc_t * img;
    if(!g_fast) return false;
    if(t->type == LV_DRAW_DSC_TYPE_BLUR) return true;
    if(!plain_image(t, &img)) return false;
    if(img->header.flags & LV_DRAW_ORTHO_IMAGE_FLAG_NATIVE) return true;
    return (int64_t)img->header.w * img->header.h * lv_color_format_get_size(img->header.cf) >=
           LV_DRAW_ORTHO_ALONE_BYTES;
}

static void execute_alone(ortho_unit_t * u, lv_draw_task_t * t)
{
    ortho_t o;
    if(!ortho_from_matrix(&t->matrix, &o)) {
        g_stats.tasks_refused++;
        return;
    }
    job_t j;
    j.t = t;
    if(!task_todo(t, &o, &j.todo)) return;
    if(g_fast) {
        if(try_fill(t, &o, &j.todo)) return;
        if(try_image(t, &o, &j.todo)) return;
    }
    find_hole(&j);
    draw_jobs(u, &j, 1, &o);
}

static bool same_matrix(const lv_draw_task_t * a, const lv_draw_task_t * b)
{
    return lv_memcmp(&a->matrix, &b->matrix, sizeof(lv_matrix_t)) == 0;
}

static int32_t evaluate(lv_draw_unit_t * draw_unit, lv_draw_task_t * task)
{
    LV_UNUSED(draw_unit);
    ortho_t o;
    if(!ortho_from_matrix(&task->matrix, &o)) return 0;
    /* An identity matrix is the software renderer's own case -- unless a
     * window has to be kept clear, which only this unit knows about. */
    if(ortho_is_identity(&o) && !has_window(task->target_layer)) return 0;
    switch(task->type) {
        case LV_DRAW_DSC_TYPE_FILL:
        case LV_DRAW_DSC_TYPE_BORDER:
        case LV_DRAW_DSC_TYPE_BOX_SHADOW:
        case LV_DRAW_DSC_TYPE_LETTER:
        case LV_DRAW_DSC_TYPE_LABEL:
        case LV_DRAW_DSC_TYPE_IMAGE:
        case LV_DRAW_DSC_TYPE_ARC:
        case LV_DRAW_DSC_TYPE_LINE:
        case LV_DRAW_DSC_TYPE_BLUR:
        case LV_DRAW_DSC_TYPE_TRIANGLE:
        case LV_DRAW_DSC_TYPE_LAYER:
        case LV_DRAW_DSC_TYPE_MASK_RECTANGLE:
#if LV_USE_VECTOR_GRAPHIC && LV_USE_THORVG
        case LV_DRAW_DSC_TYPE_VECTOR:
#endif
            break;
        default:
            return 0;
    }
    /* Nothing else can draw a turned task, so take it whatever was offered. */
    task->preference_score = 0;
    task->preferred_draw_unit_id = DRAW_UNIT_ID_ORTHO;
    return 1;
}

static int32_t dispatch(lv_draw_unit_t * draw_unit, lv_layer_t * layer)
{
    ortho_unit_t * u = (ortho_unit_t *)draw_unit;
    if(u->task_act) return 0;

    lv_draw_task_t * t = lv_draw_get_available_task(layer, NULL, DRAW_UNIT_ID_ORTHO);
    if(t == NULL) return LV_DRAW_UNIT_IDLE;

    /* LVGL offers each task the moment it is created. Taking them one by one
     * would turn every widget's pixels separately; waiting until the layer is
     * complete (or a batch is full) turns them once. */
    if(!layer->all_tasks_added && g_batch) {
        int32_t waiting = 0;
        for(lv_draw_task_t * c = t; c; c = c->next) {
            if(c->state == LV_DRAW_TASK_STATE_WAITING && c->preferred_draw_unit_id == DRAW_UNIT_ID_ORTHO) waiting++;
        }
        if(waiting < LV_DRAW_ORTHO_MAX_BATCH) return LV_DRAW_UNIT_IDLE;
    }
    if(lv_draw_layer_alloc_buf(layer) == NULL) return LV_DRAW_UNIT_IDLE;

    /* A batch is a run of tasks with nothing unfinished before it, so drawing
     * them in list order is the order LVGL itself would have drawn them. */
    lv_draw_task_t * head = layer->draw_task_head;
    while(head && head->state == LV_DRAW_TASK_STATE_FINISHED) head = head->next;

    ortho_t o;
    int32_t n = 0;
    if(g_batch && head == t && !fast_alone(t) && ortho_from_matrix(&t->matrix, &o)) {
        for(lv_draw_task_t * c = t; c && n < LV_DRAW_ORTHO_MAX_BATCH; c = c->next) {
            if(c->state != LV_DRAW_TASK_STATE_WAITING || c->preferred_draw_unit_id != DRAW_UNIT_ID_ORTHO) break;
            if(fast_alone(c) || !same_matrix(c, t)) break;
            u->jobs[n].t = c;
            if(!task_todo(c, &o, &u->jobs[n].todo)) {
                /* Nothing to draw: still part of the run, and finished with it. */
                lv_area_set(&u->jobs[n].todo, 0, 0, -1, -1);
            }
            n++;
        }
    }

    if(n <= 1) {
        t->state = LV_DRAW_TASK_STATE_IN_PROGRESS;
        t->draw_unit = draw_unit;
        u->task_act = t;
        execute_alone(u, t);
        t->state = LV_DRAW_TASK_STATE_FINISHED;
        u->task_act = NULL;
        lv_draw_dispatch_request();
        return 1;
    }

    /* Drop the tasks with nothing to draw from the jobs, keep them finished. */
    int32_t m = 0;
    for(int32_t i = 0; i < n; i++) {
        lv_draw_task_t * c = u->jobs[i].t;
        c->state = LV_DRAW_TASK_STATE_IN_PROGRESS;
        c->draw_unit = draw_unit;
        if(u->jobs[i].todo.x2 < u->jobs[i].todo.x1) continue;
        u->jobs[m] = u->jobs[i];
        find_hole(&u->jobs[m]);
        m++;
    }
    u->task_act = t;
    if(m > 0) draw_jobs(u, u->jobs, m, &o);
    g_stats.batches++;
    /* jobs[] was compacted, so walk the list again for the states. */
    lv_draw_task_t * c = t;
    for(int32_t i = 0; i < n; i++, c = c->next) c->state = LV_DRAW_TASK_STATE_FINISHED;
    u->task_act = NULL;
    lv_draw_dispatch_request();
    return n;
}

/* The tile is where every widget is drawn and every pixel of it is turned
 * out of, so it has to be the fastest memory there is. On an ESP32-P4 that is
 * internal RAM: LVGL's own allocator there (ESPHome's lv_malloc_core) asks
 * PSRAM first, which would send all of the drawing through the same bus the
 * frame buffer and the panel's scan-out already share -- the very traffic
 * this unit exists to avoid. PSRAM only if internal RAM cannot hold it. */
#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
static void * tile_alloc(size_t size, bool * internal)
{
    void * p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    *internal = p != NULL;
    return p ? p : heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
static void tile_free(void * p)
{
    heap_caps_free(p);
}
#else
static void * tile_alloc(size_t size, bool * internal)
{
    *internal = false;
    return lv_malloc(size);
}
static void tile_free(void * p)
{
    lv_free(p);
}
#endif

static uint8_t * tile_aligned(void * p)
{
    return (uint8_t *)(((uintptr_t)p + TILE_ALIGN - 1) & ~(uintptr_t)(TILE_ALIGN - 1));
}

/* The second tile, when a turner can use it: internal RAM or nothing, since a
 * tile in PSRAM would put all the drawing on the bus the turn is meant to
 * keep it off. */
static void second_tile(ortho_unit_t * u)
{
    if(u->tile[1] || g_turn == NULL) return;
    bool internal = false;
    void * p = tile_alloc(LV_DRAW_ORTHO_TILE_BYTES + TILE_ALIGN, &internal);
#if !defined(ESP_PLATFORM)
    internal = true;   /* a PC has one kind of memory */
#endif
    if(p && !internal) {
        tile_free(p);
        p = NULL;
    }
    if(p == NULL) {
        LV_LOG_WARN("ortho: no internal RAM for a second tile; the CPU waits for each turn");
        return;
    }
    u->tile_alloc[1] = p;
    u->tile[1] = tile_aligned(p);
}

static int32_t delete_cb(lv_draw_unit_t * draw_unit)
{
    ortho_unit_t * u = (ortho_unit_t *)draw_unit;
    fence();
    for(int i = 0; i < 2; i++) {
        if(u->tile_alloc[i]) tile_free(u->tile_alloc[i]);
        u->tile_alloc[i] = NULL;
        u->tile[i] = NULL;
    }
    lv_free(u->jobs);
    u->jobs = NULL;
    g_unit = NULL;
    return 0;
}

void lv_draw_ortho_init(void)
{
    if(g_unit) return;
    ortho_unit_t * u = lv_draw_create_unit(sizeof(ortho_unit_t));
    u->base.name = "ORTHO";
    u->base.evaluate_cb = evaluate;
    u->base.dispatch_cb = dispatch;
    u->base.delete_cb = delete_cb;
    bool internal = false;
    u->tile_alloc[0] = tile_alloc(LV_DRAW_ORTHO_TILE_BYTES + TILE_ALIGN, &internal);
    LV_ASSERT_MALLOC(u->tile_alloc[0]);
    g_tile_internal = internal;
    u->tile[0] = tile_aligned(u->tile_alloc[0]);
    u->jobs = lv_malloc(sizeof(job_t) * LV_DRAW_ORTHO_MAX_BATCH);
    LV_ASSERT_MALLOC(u->jobs);
    g_unit = u;
    second_tile(u);
}

void lv_draw_ortho_set_turner(lv_draw_ortho_turn_cb_t turn, lv_draw_ortho_wait_cb_t wait)
{
    fence();
    g_turn = wait ? turn : NULL;
    g_wait = g_turn ? wait : NULL;
    if(g_unit) {
        g_unit->ticket[0] = g_unit->ticket[1] = 0;
        g_unit->next = 0;
        second_tile(g_unit);
    }
}

void lv_draw_ortho_set_copy_cb(lv_draw_ortho_copy_cb_t cb)
{
    g_copy_cb = cb;
}

void lv_draw_ortho_set_fast_paths(bool enable)
{
    g_fast = enable;
}

void lv_draw_ortho_set_batching(bool enable)
{
    g_batch = enable;
}

const lv_draw_ortho_stats_t * lv_draw_ortho_get_stats(void)
{
    g_stats.tile_internal = g_tile_internal;
    g_stats.tiles_in_turn = g_unit && g_unit->tile[1] && g_turn ? 2 : 1;
    return &g_stats;
}

void lv_draw_ortho_set_window(lv_display_t * disp, const lv_area_t * native_area)
{
    if(disp == NULL || native_area == NULL) {
        g_win_disp = NULL;
        return;
    }
    if(lv_display_get_render_mode(disp) != LV_DISPLAY_RENDER_MODE_DIRECT) {
        LV_LOG_WARN("ortho: a video window needs LV_DISPLAY_RENDER_MODE_DIRECT; ignored");
        g_win_disp = NULL;
        return;
    }
    g_win = *native_area;
    g_win_disp = disp;
}

void lv_draw_ortho_native_area(lv_display_t * disp, const lv_area_t * area, lv_area_t * native_area)
{
    *native_area = *area;
    lv_display_rotate_area(disp, native_area);
}

void lv_draw_ortho_reset_stats(void)
{
    lv_memzero(&g_stats, sizeof(g_stats));
}

#endif /* LV_USE_DRAW_ORTHO */
