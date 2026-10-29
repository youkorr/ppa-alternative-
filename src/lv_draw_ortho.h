/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 youkorr
 * Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
 */

/**
 * @file lv_draw_ortho.h
 *
 * A draw unit that renders straight into a display's NATIVE orientation when
 * the display is turned by 90, 180 or 270 degrees.
 *
 * LVGL normally renders a turned display upright and turns the finished
 * buffer in the flush callback: every changed pixel is written twice, and the
 * second write is a transpose, which is the slowest thing a memory bus can be
 * asked to do. LVGL 9 already has the other half of the answer:
 * `lv_display_set_matrix_rotation(disp, true)` hands every draw task a matrix
 * and lets a draw unit render through it -- but only the vector GPU units read
 * that matrix, so on a plain software renderer the option draws nothing.
 *
 * This unit reads it. A matrix that only turns by quarter turns is a
 * permutation of pixels, so:
 *  - a solid fill is drawn as the native rectangle it becomes, at the cost of
 *    an unturned fill;
 *  - an opaque image is copied with its turn in ONE pass from its source to
 *    the frame buffer, instead of a copy and then a turn;
 *  - an image whose pixels are already stored in the panel's orientation
 *    (`LV_DRAW_ORTHO_IMAGE_FLAG_NATIVE`) is copied row by row with no turn at
 *    all -- which is what a video source can produce for free;
 *  - everything else (text, borders, shadows, arcs, masks, layers...) is drawn
 *    by LVGL's own software renderer into a small upright tile and put back
 *    turned, so its pixels are exactly the software renderer's.
 */

#ifndef LV_DRAW_ORTHO_H
#define LV_DRAW_ORTHO_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

#ifndef LV_USE_DRAW_ORTHO
#define LV_USE_DRAW_ORTHO 1
#endif

#if LV_USE_DRAW_ORTHO

#if !LV_DRAW_TRANSFORM_USE_MATRIX
#error "lv_draw_ortho needs LV_DRAW_TRANSFORM_USE_MATRIX 1 (and so LV_USE_MATRIX and LV_USE_FLOAT)"
#endif

/** Bytes of the upright tile the general path draws into. Small enough to
 *  stay in internal RAM and in the data cache, large enough that a line of
 *  text is one or two tiles. */
#ifndef LV_DRAW_ORTHO_TILE_BYTES
#define LV_DRAW_ORTHO_TILE_BYTES (32 * 1024)
#endif

/** An image carrying this flag stores its pixels in the panel's native
 *  orientation: `header.w` x `header.h` is still its size as the UI sees it,
 *  but row r of the data is native row r of what it will become on the glass.
 *  Drawn on a turned display, unscaled and opaque, it is a plain row copy. */
#define LV_DRAW_ORTHO_IMAGE_FLAG_NATIVE LV_IMAGE_FLAGS_USER1

/** Counters, so a board can say where its time goes. */
typedef struct {
    uint32_t fills_direct;     /**< solid fills drawn as native rectangles */
    uint32_t images_turned;    /**< images copied with their turn in one pass */
    uint32_t images_native;    /**< pre-turned images copied row by row */
    uint32_t tiles;            /**< upright tiles drawn by the software renderer */
    uint32_t tasks_general;    /**< tasks drawn through tiles */
    uint32_t tasks_refused;    /**< tasks whose matrix was not a quarter turn */
    uint64_t bytes_turned;     /**< bytes moved through a turn */
    uint64_t bytes_straight;   /**< bytes moved without one */
} lv_draw_ortho_stats_t;

/** Called with a straight (unturned) copy so a platform can hand it to DMA.
 *  Must copy `h` rows of `row_bytes` and return true, or return false to let
 *  the CPU do it. */
typedef bool (*lv_draw_ortho_copy_cb_t)(void * dest, int32_t dest_stride,
                                        const void * src, int32_t src_stride,
                                        int32_t row_bytes, int32_t h);

/** Create the draw unit. Call after `lv_init()`. */
void lv_draw_ortho_init(void);

void lv_draw_ortho_set_copy_cb(lv_draw_ortho_copy_cb_t cb);

/** Turn the fast paths off, so every task goes through tiles. For tests. */
void lv_draw_ortho_set_fast_paths(bool enable);

const lv_draw_ortho_stats_t * lv_draw_ortho_get_stats(void);
void lv_draw_ortho_reset_stats(void);

#endif /* LV_USE_DRAW_ORTHO */

#ifdef __cplusplus
}
#endif

#endif /* LV_DRAW_ORTHO_H */
