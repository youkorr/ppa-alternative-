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
#include <stdlib.h>
#include <string.h>

#include "esp_rom_sys.h"

p4fake_t g_p4;
unsigned long long g_p4_pixels;
int g_p4_runs;

/* A client: its own callback, as on the board. */
struct ppa_client_t {
    ppa_event_callbacks_t cbs;
    int pending_max;
};

/* Non-blocking transactions not yet run, in the order they were queued. */
#define QUEUE 64
static struct {
    ppa_client_handle_t c;
    ppa_srm_oper_config_t op;
} g_queue[QUEUE];
static int g_head, g_count;

esp_err_t ppa_register_client(const ppa_client_config_t * config, ppa_client_handle_t * ret_client)
{
    struct ppa_client_t * c = calloc(1, sizeof(*c));
    if(c == NULL) return ESP_ERR_NO_MEM;
    c->pending_max = config && config->max_pending_trans_num ? (int)config->max_pending_trans_num : 1;
    *ret_client = c;
    return ESP_OK;
}

esp_err_t ppa_unregister_client(ppa_client_handle_t c)
{
    for(int i = 0; i < g_count; i++) if(g_queue[(g_head + i) % QUEUE].c == c) return ESP_ERR_INVALID_STATE;
    free(c);
    return ESP_OK;
}

esp_err_t ppa_client_register_event_callbacks(ppa_client_handle_t c, const ppa_event_callbacks_t * cbs)
{
    c->cbs = *cbs;
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

/* The simulated P4 (test/p4sim) does not count the PPA's work as the CPU's:
 * `addi x0, x0, 2047` and 2046 tell its plugin where the DMA starts and stops. */
#if defined(P4SIM)
#define DMA_BEGIN() __asm__ volatile("addi x0, x0, 2047" ::: "memory")
#define DMA_END() __asm__ volatile("addi x0, x0, 2046" ::: "memory")
#else
#define DMA_BEGIN() do {} while(0)
#define DMA_END() do {} while(0)
#endif

static void run(const ppa_srm_oper_config_t * op)
{
    const int px = op->in.srm_cm == PPA_SRM_COLOR_MODE_RGB565 ? 2 : 3;
    const uint32_t bw = op->in.block_w, bh = op->in.block_h;
    const bool across = op->rotation_angle == PPA_SRM_ROTATION_ANGLE_90 ||
                        op->rotation_angle == PPA_SRM_ROTATION_ANGLE_270;
    const uint32_t ow = across ? bh : bw, oh = across ? bw : bh;
    g_p4_pixels += (unsigned long long)ow * oh;
    g_p4_runs++;
    DMA_BEGIN();
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
    DMA_END();
}

static void finish_one(void)
{
    if(g_count == 0) return;
    ppa_client_handle_t c = g_queue[g_head].c;
    run(&g_queue[g_head].op);
    void * user = g_queue[g_head].op.user_data;
    g_head = (g_head + 1) % QUEUE;
    g_count--;
    ppa_event_data_t e;
    if(c->cbs.on_trans_done) c->cbs.on_trans_done(c, &e, user);
}

void p4fake_run_all(void)
{
    while(g_count) finish_one();
}

static const void * g_watch;
static int g_watched;

void p4fake_watch(const void * buf)
{
    g_watch = buf;
    g_watched = 0;
}

int p4fake_watched(void)
{
    return g_watched;
}

int p4fake_pending(void)
{
    return g_count;
}

void esp_rom_delay_us(uint32_t us)
{
    (void)us;
    finish_one();
}

esp_err_t ppa_do_scale_rotate_mirror(ppa_client_handle_t c, const ppa_srm_oper_config_t * op)
{
    g_p4.srm_count++;
    g_p4.last = *op;
    if(g_watch && op->in.buffer == g_watch) g_watched++;
    /* ppa.rst: out.buffer and out.buffer_size aligned to the L2 cache line. */
    if(((uintptr_t)op->out.buffer % 128) || (op->out.buffer_size % 128)) return ESP_ERR_INVALID_ARG;
    if(op->scale_x != 1.0f || op->scale_y != 1.0f) return ESP_ERR_NOT_SUPPORTED;
    const int px = op->in.srm_cm == PPA_SRM_COLOR_MODE_RGB565 ? 2 : 3;
    const uint32_t bw = op->in.block_w, bh = op->in.block_h;
    const bool across = op->rotation_angle == PPA_SRM_ROTATION_ANGLE_90 ||
                        op->rotation_angle == PPA_SRM_ROTATION_ANGLE_270;
    const uint32_t ow = across ? bh : bw, oh = across ? bw : bh;
    if(op->in.block_offset_x + bw > op->in.pic_w || op->in.block_offset_y + bh > op->in.pic_h)
        return ESP_ERR_INVALID_ARG;
    if(op->out.block_offset_x + ow > op->out.pic_w || op->out.block_offset_y + oh > op->out.pic_h)
        return ESP_ERR_INVALID_ARG;
    if((size_t)op->out.pic_w * op->out.pic_h * px > op->out.buffer_size + 128) return ESP_ERR_INVALID_ARG;
    /* ppa_srm.c asserts the rows it invalidates lie inside buffer_size. */
    if((size_t)(op->out.block_offset_y + oh) * op->out.pic_w * px > op->out.buffer_size) return ESP_ERR_INVALID_ARG;
    if(op->mode == PPA_TRANS_MODE_NON_BLOCKING) {
        int pending = 0;
        for(int i = 0; i < g_count; i++) if(g_queue[(g_head + i) % QUEUE].c == c) pending++;
        if(pending >= c->pending_max || g_count == QUEUE) return ESP_ERR_INVALID_STATE;
        g_queue[(g_head + g_count) % QUEUE].c = c;
        g_queue[(g_head + g_count) % QUEUE].op = *op;
        g_count++;
        return ESP_OK;
    }
    /* A blocking one waits for the queue ahead of it: the engine is FIFO. */
    p4fake_run_all();
    run(op);
    return ESP_OK;
}
