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
int g_p4_dig734;
static int g_stall_next;

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
    ppa_fill_oper_config_t fill;
    int is_fill;
    int stuck;
    int qid;
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
/* The timeline the simulation replays: each transaction queued, and each
 * moment the CPU needed one finished. p4cache.c snapshots its counts at the
 * marker, in the same order, and p4sim.py puts the two together. */
p4fake_event_t g_p4_ev[P4FAKE_EVENTS];
int g_p4_nev;
static int g_qid;
static void note(char type, int id, unsigned px, int fill)
{
    if(g_p4_nev < P4FAKE_EVENTS) {
        g_p4_ev[g_p4_nev].type = type;
        g_p4_ev[g_p4_nev].id = id;
        g_p4_ev[g_p4_nev].px = px;
        g_p4_ev[g_p4_nev].fill = fill;
    }
    g_p4_nev++;
    __asm__ volatile("addi x0, x0, 2040" ::: "memory");
}
#define NOTE_QUEUED(id, px, fill) note('Q', id, px, fill)
#define NOTE_WAITED(id) note('R', id, 0, 0)
#else
#define DMA_BEGIN() do {} while(0)
#define DMA_END() do {} while(0)
static int g_qid;
#define NOTE_QUEUED(id, px, fill) do {} while(0)
#define NOTE_WAITED(id) do {} while(0)
#endif

int g_p4_fills;
unsigned long long g_p4_fill_pixels;

/* A fill, as the blend engine does it: the colour narrowed by TRUNCATING to
 * RGB565 -- an assumption, made harmless by lv_draw_ortho_ppa.c handing it a
 * colour already at RGB565's precision. */
static void run_fill(const ppa_fill_oper_config_t * f)
{
    const int px = f->out.fill_cm == PPA_FILL_COLOR_MODE_RGB565 ? 2 : 3;
    const uint32_t c = f->fill_argb_color.val;
    const uint8_t r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
    const uint16_t c16 = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    g_p4_pixels += (unsigned long long)f->fill_block_w * f->fill_block_h;
    g_p4_fill_pixels += (unsigned long long)f->fill_block_w * f->fill_block_h;
    g_p4_fills++;
    DMA_BEGIN();
    for(uint32_t y = 0; y < f->fill_block_h; y++) {
        uint8_t * d = (uint8_t *)f->out.buffer + ((f->out.block_offset_y + y) * f->out.pic_w + f->out.block_offset_x) * px;
        for(uint32_t x = 0; x < f->fill_block_w; x++, d += px) {
            if(px == 2) memcpy(d, &c16, 2);
            else {
                d[0] = b;
                d[1] = g;
                d[2] = r;
            }
        }
    }
    DMA_END();
}

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
    if(g_count == 0 || g_queue[g_head].stuck) return;
    ppa_client_handle_t c = g_queue[g_head].c;
    NOTE_WAITED(g_queue[g_head].qid);
    if(g_queue[g_head].is_fill) run_fill(&g_queue[g_head].fill);
    else run(&g_queue[g_head].op);
    void * user = g_queue[g_head].op.user_data;
    g_head = (g_head + 1) % QUEUE;
    g_count--;
    ppa_event_data_t e;
    if(c->cbs.on_trans_done) c->cbs.on_trans_done(c, &e, user);
}

void p4fake_run_all(void)
{
    while(g_count && !g_queue[g_head].stuck) finish_one();
}

void p4fake_stall_next(void)
{
    g_stall_next = 1;
}

void p4fake_reset(void)
{
    g_head = g_count = 0;
    g_stall_next = 0;
}


/* The hardware fault behind esp-idf #19023 and #19096, as the driver of
 * ESP-IDF v5.5.6 meets it, written from ppa_srm.c rather than shared with
 * lv_draw_ortho_ppa.c, so the two cannot agree by copying each other. The
 * driver decides whether to bypass the macro-block order from the INPUT
 * block's width and height; the hardware, at 90 and 270, needs it decided
 * from the turned ones (the fix on master, 469aa16). Where it needed the
 * bypass and was not given it, the DMA never raises its end: the transaction
 * never finishes. Unscaled RGB565/RGB888, 16x16 macro blocks (silicon v1.x). */
static int mb_bypass(uint32_t w_out, uint32_t h_in, uint32_t bits)
{
    const uint32_t wdiv = bits == 16 ? 64 : 32;
    uint32_t wl = w_out % wdiv, hl = h_in % 16;
    if(wl == 0) wl = wdiv;
    if(hl == 0) hl = 16;
    return (w_out > wdiv || h_in > 16) && wl * hl * bits < 12 * 128;
}

static int never_finishes(const ppa_srm_oper_config_t * op)
{
    if(op->rotation_angle != PPA_SRM_ROTATION_ANGLE_90 && op->rotation_angle != PPA_SRM_ROTATION_ANGLE_270) return 0;
    const uint32_t bits = op->out.srm_cm == PPA_SRM_COLOR_MODE_RGB565 ? 16 : 24;
    const int given = mb_bypass(op->in.block_w, op->in.block_h, bits);
    const int needed = mb_bypass(op->in.block_h, op->in.block_w, bits);
    return needed && !given;
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
        const int stuck = never_finishes(op) || g_stall_next;
        g_p4_dig734 += never_finishes(op);
        g_stall_next = 0;
        g_queue[(g_head + g_count) % QUEUE].c = c;
        g_queue[(g_head + g_count) % QUEUE].op = *op;
        g_queue[(g_head + g_count) % QUEUE].is_fill = 0;
        g_queue[(g_head + g_count) % QUEUE].stuck = stuck;
        g_queue[(g_head + g_count) % QUEUE].qid = ++g_qid;
        g_count++;
        NOTE_QUEUED(g_qid, ow * oh, 0);
        return ESP_OK;
    }
    /* A blocking one waits for the queue ahead of it: the engine is FIFO. A
     * stuck one ahead of it, or itself, and it never returns: the driver
     * waits with portMAX_DELAY. Here that is a failed test instead. */
    p4fake_run_all();
    if(g_count || never_finishes(op)) {
        g_p4_dig734 += never_finishes(op);
        fprintf(stderr, "p4fake: a blocking PPA transaction that would never return\n");
        return ESP_FAIL;
    }
    NOTE_QUEUED(++g_qid, ow * oh, 0);
    NOTE_WAITED(g_qid);
    run(op);
    return ESP_OK;
}

esp_err_t ppa_do_fill(ppa_client_handle_t c, const ppa_fill_oper_config_t * f)
{
    /* ppa_fill.c's checks that matter here. */
    if(((uintptr_t)f->out.buffer % 128) || (f->out.buffer_size % 128)) return ESP_ERR_INVALID_ARG;
    const int px = f->out.fill_cm == PPA_FILL_COLOR_MODE_RGB565 ? 2 : 3;
    if((size_t)f->out.pic_w * f->out.pic_h * px > f->out.buffer_size) return ESP_ERR_INVALID_ARG;
    if(f->fill_block_w == 0 || f->fill_block_h == 0 || f->fill_block_w > f->out.pic_w - f->out.block_offset_x ||
       f->fill_block_h > f->out.pic_h - f->out.block_offset_y)
        return ESP_ERR_INVALID_ARG;
    if(f->mode == PPA_TRANS_MODE_NON_BLOCKING) {
        if(g_count == QUEUE) return ESP_ERR_INVALID_STATE;
        const int i = (g_head + g_count) % QUEUE;
        g_queue[i].c = c;
        g_queue[i].fill = *f;
        g_queue[i].is_fill = 1;
        g_queue[i].stuck = g_stall_next;
        g_stall_next = 0;
        g_queue[i].qid = ++g_qid;
        g_count++;
        NOTE_QUEUED(g_qid, f->fill_block_w * f->fill_block_h, 1);
        return ESP_OK;
    }
    p4fake_run_all();
    if(g_count) return ESP_FAIL;
    NOTE_QUEUED(++g_qid, f->fill_block_w * f->fill_block_h, 1);
    NOTE_WAITED(g_qid);
    run_fill(f);
    return ESP_OK;
}
