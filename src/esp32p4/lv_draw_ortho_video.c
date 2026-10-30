/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 youkorr
 * Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
 */

#include "lv_draw_ortho_video.h"
#include "esp_cache.h"

/* The PPA writes by DMA and its output buffer and size must be aligned to the
 * L2 cache line (ESP-IDF's ppa.rst, "Buffer Alignment"). */
#ifdef CONFIG_CACHE_L2_CACHE_LINE_SIZE
#define LINE CONFIG_CACHE_L2_CACHE_LINE_SIZE
#else
#define LINE 128
#endif

esp_err_t lv_draw_ortho_video_init(lv_draw_ortho_video_t * v, lv_display_t * disp)
{
    lv_memzero(v, sizeof(*v));
    v->disp = disp;
    ppa_client_config_t cfg = {0};
    cfg.oper_type = PPA_OPERATION_SRM;
    cfg.max_pending_trans_num = 1;
    return ppa_register_client(&cfg, &v->client);
}

void lv_draw_ortho_video_place(lv_draw_ortho_video_t * v, const lv_area_t * ui_area)
{
    v->ui_area = *ui_area;
    lv_draw_ortho_native_area(v->disp, ui_area, &v->native);
    lv_draw_ortho_set_window(v->disp, &v->native);
    v->placed = true;
}

/* LVGL turns content one way and the PPA counts the other: the PPA's
 * "counter-clockwise" is measured with y pointing up, so on a screen its 90 is
 * a clockwise quarter turn. LVGL's 90 is the PPA's 270, which is the pairing
 * Espressif's esp_lvgl_port ships and M5Stack's Tab5 runs. */
static ppa_srm_rotation_angle_t ppa_angle(lv_display_rotation_t r)
{
    switch(r) {
        case LV_DISPLAY_ROTATION_90:
            return PPA_SRM_ROTATION_ANGLE_270;
        case LV_DISPLAY_ROTATION_180:
            return PPA_SRM_ROTATION_ANGLE_180;
        case LV_DISPLAY_ROTATION_270:
            return PPA_SRM_ROTATION_ANGLE_90;
        default:
            return PPA_SRM_ROTATION_ANGLE_0;
    }
}

esp_err_t lv_draw_ortho_video_show(lv_draw_ortho_video_t * v, const void * frame, uint32_t w, uint32_t h,
                                   lv_color_format_t cf, bool mirror_x, bool mirror_y)
{
    if(!v->placed || frame == NULL || w == 0 || h == 0) return ESP_ERR_INVALID_STATE;
    lv_draw_buf_t * fb = lv_display_get_buf_active(v->disp);
    if(fb == NULL || fb->header.cf != cf) return ESP_ERR_INVALID_ARG;
    const ppa_srm_color_mode_t cm = cf == LV_COLOR_FORMAT_RGB565 ? PPA_SRM_COLOR_MODE_RGB565
                                    : cf == LV_COLOR_FORMAT_RGB888 ? PPA_SRM_COLOR_MODE_RGB888
                                    : (ppa_srm_color_mode_t) - 1;
    if((int)cm == -1) return ESP_ERR_NOT_SUPPORTED;

    const uint32_t px = lv_color_format_get_size(cf);
    const uint32_t stride = fb->header.stride;
    const uint32_t fb_h = fb->header.h;
    const lv_display_rotation_t rot = lv_display_get_rotation(v->disp);
    const bool across = rot == LV_DISPLAY_ROTATION_90 || rot == LV_DISPLAY_ROTATION_270;

    /* Clean and drop every cache line of the window's rows first. LVGL writes
     * right up to the window's edge, and a line it shares with the window
     * holds stale video: written back later, it would put that over the new
     * frame. Writing back before the PPA runs, and invalidating at the same
     * time, leaves nothing for the CPU to put back. */
    uintptr_t a = (uintptr_t)fb->data + (uintptr_t)v->native.y1 * stride;
    uintptr_t b = (uintptr_t)fb->data + (uintptr_t)(v->native.y2 + 1) * stride;
    a &= ~(uintptr_t)(LINE - 1);
    b = (b + LINE - 1) & ~(uintptr_t)(LINE - 1);
    esp_err_t err = esp_cache_msync((void *)a, b - a,
                                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
    if(err != ESP_OK) return err;

    ppa_srm_oper_config_t op;
    lv_memzero(&op, sizeof(op));
    op.in.buffer = frame;
    op.in.pic_w = w;
    op.in.pic_h = h;
    op.in.block_w = w;
    op.in.block_h = h;
    op.in.srm_cm = cm;
    op.out.buffer = fb->data;
    op.out.buffer_size = (stride * fb_h) & ~(uint32_t)(LINE - 1);
    op.out.pic_w = stride / px;
    op.out.pic_h = fb_h;
    op.out.block_offset_x = v->native.x1;
    op.out.block_offset_y = v->native.y1;
    op.out.srm_cm = cm;
    op.rotation_angle = ppa_angle(rot);
    /* The scale applies to the frame before it turns, so it is the UI's size
     * over the frame's, whatever the angle. */
    op.scale_x = (float)lv_area_get_width(&v->ui_area) / (float)w;
    op.scale_y = (float)lv_area_get_height(&v->ui_area) / (float)h;
    /* NOT VERIFIED ON A BOARD: whether the PPA mirrors before or after it
     * turns. This assumes after (the order of its name, scale-rotate-mirror),
     * so a mirror asked for in the upright frame changes axis at 90 and 270.
     * If a mirrored camera comes out flipped the other way on a turned panel,
     * this is the line. */
    op.mirror_x = across ? mirror_y : mirror_x;
    op.mirror_y = across ? mirror_x : mirror_y;
    op.mode = PPA_TRANS_MODE_BLOCKING;
    return ppa_do_scale_rotate_mirror(v->client, &op);
}

void lv_draw_ortho_video_deinit(lv_draw_ortho_video_t * v)
{
    lv_draw_ortho_set_window(NULL, NULL);
    if(v->client) ppa_unregister_client(v->client);
    v->client = NULL;
    v->placed = false;
}
