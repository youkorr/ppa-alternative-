/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 youkorr
 * Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
 */

/**
 * @file lv_draw_ortho_ppa.h
 *
 * lv_draw_ortho's turns done by the ESP32-P4's PPA instead of its CPU.
 *
 * lv_draw_ortho draws every widget upright into a tile in internal RAM and
 * puts the tile into the frame buffer turned. Simulated on a P4
 * (test/p4sim), the drawing reads and writes no PSRAM at all; the turn, done
 * by the CPU, is what costs -- five instructions a pixel and every frame-buffer
 * line through the cache. The PPA turns a block by DMA while the CPU goes on:
 * with this, the CPU draws the next tile while the PPA turns the last one.
 *
 * RGB565 and RGB888 only; anything else, a buffer the PPA cannot write, or a
 * full queue, and that turn is the CPU's as before.
 */
#ifndef LV_DRAW_ORTHO_PPA_H
#define LV_DRAW_ORTHO_PPA_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lv_draw_ortho.h"
#include "esp_err.h"

/** Register a PPA client and hand lv_draw_ortho's turns to it. */
esp_err_t lv_draw_ortho_ppa_init(void);

/** Hand the turns back to the CPU and release the PPA. */
void lv_draw_ortho_ppa_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* LV_DRAW_ORTHO_PPA_H */
