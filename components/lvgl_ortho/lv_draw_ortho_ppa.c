/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 youkorr
 * Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
 */

#include "lv_draw_ortho_ppa.h"
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_rom_sys.h"

/* The PPA's output must start on, and its size be a multiple of, the L2
 * cache line (ESP-IDF's ppa.rst, "Buffer Alignment"). */
#ifdef CONFIG_CACHE_L2_CACHE_LINE_SIZE
#define LINE CONFIG_CACHE_L2_CACHE_LINE_SIZE
#else
#define LINE 128
#endif

/* Two tiles of up to four parts each and the read-back of a third: what can be
 * queued at once. A full queue is declined and the CPU does that turn. */
#define PENDING 16

static ppa_client_handle_t g_client;
static uint32_t g_queued;            /* tickets handed out */
static volatile uint32_t g_done;     /* counted by the PPA's interrupt */

static bool on_done(ppa_client_handle_t client, ppa_event_data_t * event, void * user)
{
    (void)client;
    (void)event;
    (void)user;
    g_done++;
    return false;
}

/* As in lv_draw_ortho_video.c: LVGL's 90 and the PPA's 90 are the same turn. */
static ppa_srm_rotation_angle_t ppa_angle(lv_display_rotation_t r)
{
    switch(r) {
        case LV_DISPLAY_ROTATION_90:
            return PPA_SRM_ROTATION_ANGLE_90;
        case LV_DISPLAY_ROTATION_180:
            return PPA_SRM_ROTATION_ANGLE_180;
        case LV_DISPLAY_ROTATION_270:
            return PPA_SRM_ROTATION_ANGLE_270;
        default:
            return PPA_SRM_ROTATION_ANGLE_0;
    }
}

static uint32_t turn(const lv_draw_ortho_pic_t * src, const lv_area_t * block, const lv_draw_ortho_pic_t * dst,
                     int32_t dx, int32_t dy, lv_display_rotation_t rot, lv_color_format_t cf)
{
    const ppa_srm_color_mode_t cm = cf == LV_COLOR_FORMAT_RGB565 ? PPA_SRM_COLOR_MODE_RGB565
                                    : cf == LV_COLOR_FORMAT_RGB888 ? PPA_SRM_COLOR_MODE_RGB888
                                    : (ppa_srm_color_mode_t) - 1;
    if((int)cm == -1 || g_client == NULL || g_queued - g_done >= PENDING) return 0;
    const uint32_t px = lv_color_format_get_size(cf);
    /* The PPA counts a row as pic_w pixels: no padding in a stride. */
    if(src->stride != src->w * px || dst->stride != dst->w * px) return 0;
    if(((uintptr_t)dst->buf % LINE) != 0) return 0;

    const bool across = rot == LV_DISPLAY_ROTATION_90 || rot == LV_DISPLAY_ROTATION_270;
    const uint32_t bw = (uint32_t)lv_area_get_width(block), bh = (uint32_t)lv_area_get_height(block);
    const uint32_t rows = across ? bw : bh;
    /* The driver invalidates whole rows of the output from dy, rounded to the
     * line: they have to lie inside the buffer it is told about. */
    const uint32_t size = (dst->stride * dst->h) & ~(uint32_t)(LINE - 1);
    uintptr_t a = (uintptr_t)dst->buf + (uintptr_t)dy * dst->stride;
    uintptr_t b = a + (uintptr_t)rows * dst->stride;
    if(b > (uintptr_t)dst->buf + size) return 0;

    /* Write back and drop the output rows first. The CPU has drawn right up to
     * this block's edges, and a cache line it shares with the block holds its
     * pixels too: the driver's invalidate would throw those away, and a later
     * write-back would put stale ones over the PPA's. lv_draw_ortho_video.c
     * does the same for the same reason. */
    a &= ~(uintptr_t)(LINE - 1);
    b = (b + LINE - 1) & ~(uintptr_t)(LINE - 1);
    if(esp_cache_msync((void *)a, b - a, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE) != ESP_OK)
        return 0;

    ppa_srm_oper_config_t op;
    lv_memzero(&op, sizeof(op));
    op.in.buffer = src->buf;
    op.in.pic_w = src->w;
    op.in.pic_h = src->h;
    op.in.block_w = bw;
    op.in.block_h = bh;
    op.in.block_offset_x = (uint32_t)block->x1;
    op.in.block_offset_y = (uint32_t)block->y1;
    op.in.srm_cm = cm;
    op.out.buffer = dst->buf;
    op.out.buffer_size = size;
    op.out.pic_w = dst->w;
    op.out.pic_h = dst->h;
    op.out.block_offset_x = (uint32_t)dx;
    op.out.block_offset_y = (uint32_t)dy;
    op.out.srm_cm = cm;
    op.rotation_angle = ppa_angle(rot);
    op.scale_x = 1.0f;
    op.scale_y = 1.0f;
    op.mode = PPA_TRANS_MODE_NON_BLOCKING;
    if(ppa_do_scale_rotate_mirror(g_client, &op) != ESP_OK) return 0;
    return ++g_queued;
}

/* The PPA finishes its transactions in the order they were queued, so a
 * ticket is done once that many have finished. */
static void wait(uint32_t ticket)
{
    while((int32_t)(g_done - ticket) < 0) esp_rom_delay_us(1);
}

esp_err_t lv_draw_ortho_ppa_init(void)
{
    if(g_client) return ESP_OK;
    ppa_client_config_t cfg;
    lv_memzero(&cfg, sizeof(cfg));
    cfg.oper_type = PPA_OPERATION_SRM;
    cfg.max_pending_trans_num = PENDING;
    esp_err_t err = ppa_register_client(&cfg, &g_client);
    if(err != ESP_OK) return err;
    ppa_event_callbacks_t cbs = {.on_trans_done = on_done};
    err = ppa_client_register_event_callbacks(g_client, &cbs);
    if(err != ESP_OK) {
        ppa_unregister_client(g_client);
        g_client = NULL;
        return err;
    }
    g_queued = g_done = 0;
    lv_draw_ortho_set_turner(turn, wait);
    return ESP_OK;
}

void lv_draw_ortho_ppa_deinit(void)
{
    lv_draw_ortho_set_turner(NULL, NULL);
    if(g_client) ppa_unregister_client(g_client);
    g_client = NULL;
}
