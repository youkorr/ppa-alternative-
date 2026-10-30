/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 youkorr
 * Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
 *
 * A PPA and a cache on a PC, for the video producer. The PPA turns the way
 * the hardware does on the household's boards: its 90 is a counter-clockwise
 * quarter turn on the screen, as documented. The evidence is the rotated
 * flush of youkorr/lvgl_9.5 (lvgl_esphome.cpp, ppa_rotate_display_buf and the
 * geometry after it), which shows a correct UI on a Tab5: its software loop
 * for ESPHome's 90 sends the top-left pixel to the top-right (clockwise) and
 * its PPA path uses ANGLE_270 for that same picture. It mirrors AFTER
 * turning, which is this project's assumption and is NOT verified on a
 * board. Unscaled frames only.
 */
#include "p4fake.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

p4fake_t g_p4;

esp_err_t ppa_register_client(const ppa_client_config_t * config, ppa_client_handle_t * ret_client)
{
    (void)config;
    *ret_client = (ppa_client_handle_t)&g_p4;
    return ESP_OK;
}

esp_err_t ppa_unregister_client(ppa_client_handle_t c)
{
    (void)c;
    return ESP_OK;
}

esp_err_t esp_cache_msync(void * addr, size_t size, int flags)
{
    g_p4.msync_addr = (uintptr_t)addr;
    g_p4.msync_size = size;
    g_p4.msync_flags = flags;
    g_p4.msync_count++;
    return ESP_OK;
}

esp_err_t ppa_do_scale_rotate_mirror(ppa_client_handle_t c, const ppa_srm_oper_config_t * op)
{
    (void)c;
    g_p4.srm_count++;
    g_p4.last = *op;
    /* ppa.rst: out.buffer and out.buffer_size aligned to the L2 cache line. */
    if(((uintptr_t)op->out.buffer % 128) || (op->out.buffer_size % 128)) return ESP_ERR_INVALID_ARG;
    if(op->scale_x != 1.0f || op->scale_y != 1.0f) return ESP_ERR_NOT_SUPPORTED;
    const int px = op->in.srm_cm == PPA_SRM_COLOR_MODE_RGB565 ? 2 : 3;
    const uint32_t bw = op->in.block_w, bh = op->in.block_h;
    const bool across = op->rotation_angle == PPA_SRM_ROTATION_ANGLE_90 ||
                        op->rotation_angle == PPA_SRM_ROTATION_ANGLE_270;
    const uint32_t ow = across ? bh : bw, oh = across ? bw : bh;
    if(op->out.block_offset_x + ow > op->out.pic_w || op->out.block_offset_y + oh > op->out.pic_h)
        return ESP_ERR_INVALID_ARG;
    if((size_t)op->out.pic_w * op->out.pic_h * px > op->out.buffer_size + 128) return ESP_ERR_INVALID_ARG;
    for(uint32_t y = 0; y < oh; y++) {
        for(uint32_t x = 0; x < ow; x++) {
            uint32_t mx = op->mirror_x ? ow - 1 - x : x, my = op->mirror_y ? oh - 1 - y : y;
            uint32_t sx, sy;
            switch(op->rotation_angle) {
                case PPA_SRM_ROTATION_ANGLE_90:  /* counter-clockwise on a screen */
                    sx = bw - 1 - my;
                    sy = mx;
                    break;
                case PPA_SRM_ROTATION_ANGLE_180:
                    sx = bw - 1 - mx;
                    sy = bh - 1 - my;
                    break;
                case PPA_SRM_ROTATION_ANGLE_270: /* clockwise on a screen */
                    sx = my;
                    sy = bh - 1 - mx;
                    break;
                default:
                    sx = mx;
                    sy = my;
                    break;
            }
            const uint8_t * s = (const uint8_t *)op->in.buffer +
                                ((op->in.block_offset_y + sy) * op->in.pic_w + op->in.block_offset_x + sx) * px;
            uint8_t * d = (uint8_t *)op->out.buffer +
                          ((op->out.block_offset_y + y) * op->out.pic_w + op->out.block_offset_x + x) * px;
            memcpy(d, s, px);
        }
    }
    return ESP_OK;
}
