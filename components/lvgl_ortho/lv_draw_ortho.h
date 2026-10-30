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

/** Bytes of the upright tile the general path draws into. Every tile costs
 *  each task touching it a call into the software renderer (a label lays its
 *  text out again), so a tile too small is slower than LVGL's own way. On a
 *  PC with the test UI at 720x1280: 32 KiB 2.6 ms, 64 KiB 1.8, 128 KiB 1.45,
 *  256 KiB 1.3, against 1.7 for drawing upright and turning in flush. */
#ifndef LV_DRAW_ORTHO_TILE_BYTES
#define LV_DRAW_ORTHO_TILE_BYTES (128 * 1024)
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
    uint32_t tiles_unread;     /**< of which, painted over wholly so never read back */
    uint32_t batches;          /**< runs of tasks drawn through one set of tiles */
    uint32_t tasks_general;    /**< tasks drawn through tiles */
    uint32_t tasks_refused;    /**< tasks whose matrix was not a quarter turn */
    uint32_t window_skips;     /**< writes cut around the video window */
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

/** Draw every task on its own instead of in runs. For tests. */
void lv_draw_ortho_set_batching(bool enable);

/**
 * Keep LVGL out of one rectangle of a display's frame buffer, so a video
 * source -- a camera through the ESP32-P4's PPA, a JPEG decoder -- can write
 * its frames straight into the frame buffer, the way Espressif's own camera
 * examples reach the sensor's frame rate, while LVGL draws everything else.
 *
 * `native_area` is in the PANEL's coordinates (see lv_draw_ortho_native_area).
 * Works only in LV_DISPLAY_RENDER_MODE_DIRECT, where LVGL renders into the
 * frame buffer itself. One window; NULL clears it.
 *
 * The source and LVGL must not both be writing at once, and on a chip with a
 * data cache the source's frames must be made visible to the CPU (and LVGL's
 * written back) before the other side runs -- a cache line shared across the
 * window's edge would otherwise put stale pixels back. That is the caller's,
 * and the README says how on an ESP32-P4.
 */
void lv_draw_ortho_set_window(lv_display_t * disp, const lv_area_t * native_area);

/** Where an area of the UI (an object's coordinates) lands on the panel. */
void lv_draw_ortho_native_area(lv_display_t * disp, const lv_area_t * area, lv_area_t * native_area);

const lv_draw_ortho_stats_t * lv_draw_ortho_get_stats(void);
void lv_draw_ortho_reset_stats(void);

#endif /* LV_USE_DRAW_ORTHO */

#ifdef __cplusplus
}
#endif

#endif /* LV_DRAW_ORTHO_H */
