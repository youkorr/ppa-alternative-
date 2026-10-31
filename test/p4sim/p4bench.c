/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 youkorr
 * Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
 */

/*
 * The bench, built for the ESP32-P4's own processor (RV32IMAFC) and run in
 * QEMU under p4cache.c, which counts what the P4 would pay: instructions,
 * and the lines its caches move to and from the PSRAM.
 *
 * The memory is laid out the way ESPHome lays it out on a P4:
 *   - every malloc -- LVGL's heap, its draw buffer, the frame buffers, the
 *     pictures -- comes from the PSRAM (ESPHome's lv_malloc_core asks for
 *     SPIRAM first, and a whole screen fits nowhere else);
 *   - lv_draw_ortho's tile asks for internal RAM, as it does on the board
 *     (heap_caps_malloc with MALLOC_CAP_INTERNAL, see p4stub/esp_heap_caps.h);
 *   - the stack and LVGL's globals are internal, as ESP-IDF puts them.
 *
 * Each measured redraw sits between two markers (`addi x0, x0, N`), so the
 * plugin can say what that redraw alone cost. One redraw before each, so
 * caches, glyphs and LVGL's own state are warm, as on a running board.
 *
 * What runs on the P4's DMA engines -- the PPA's turn, the display's copy --
 * is not executed here: p4sim.py adds it from rates measured on the boards.
 */

/* picolibc hides CLOCK_MONOTONIC unless asked for POSIX timers; the clock
 * here is p4bench's own counter anyway. */
#include <time.h>
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC CLOCK_REALTIME
#endif

#define main ortho_tests_main
#include "../main.c"
#undef main

#define PHASE(n) __asm__ volatile("addi x0, x0, %0" :: "i"(n) : "memory")

/* The PSRAM: from 0x90000000, as QEMU's virt machine maps RAM there and
 * p4cache.c is told that range is the PSRAM. */
void * sbrk(ptrdiff_t incr)
{
    static uintptr_t brk = 0x90000000u;
    uintptr_t old = brk;
    brk += (uintptr_t)incr;
    return (void *)old;
}

/* time: LVGL wants a tick, the redraws here are forced with lv_refr_now. */
int clock_gettime(clockid_t id, struct timespec * ts)
{
    static long n;
    n++;
    ts->tv_sec = n / 1000;
    ts->tv_nsec = (n % 1000) * 1000000;
    return 0;
}

/* What the PPA turned in the measured redraw: p4sim.py times it at the rate
 * measured on a board. */
static void report_ppa(int phase_id, unsigned long long before, unsigned long long filled)
{
    printf("ppa %d: %llu pixels, %llu filled\n", phase_id, g_p4_pixels - before, g_p4_fill_pixels - filled);
}

static void redraw(lv_display_t * disp, lv_obj_t * obj, int phase_id)
{
    lv_obj_invalidate(obj);
    lv_refr_now(disp);            /* warm */
    lv_obj_invalidate(obj);
    const unsigned long long px = g_p4_pixels, filled = g_p4_fill_pixels;
    switch(phase_id) {
        /* The immediate has to be a constant: one marker per phase. */
#define CASE(n) case n: PHASE(n); lv_refr_now(disp); PHASE(1999); break;
        CASE(20) CASE(21) CASE(22) CASE(30) CASE(31) CASE(32) CASE(40) CASE(41) CASE(42)
        CASE(50) CASE(51) CASE(52) CASE(60) CASE(61) CASE(62) CASE(70) CASE(71) CASE(72)
#undef CASE
    }
    report_ppa(phase_id, px, filled);
}

/* lv_draw_ortho with the PPA turning (lv_draw_ortho_ppa.c, on the fake
 * PPA): the UI, or a picture when one is given. The fake's copying is the
 * PPA's and not counted (P4SIM markers); its pixels are reported. */
static void ortho_with_ppa(int32_t nw, int32_t nh, lv_display_rotation_t rot, int phase,
                           const lv_image_dsc_t * frame)
{
    if(lv_draw_ortho_ppa_init() != ESP_OK) {
        printf("no PPA\n");
        return;
    }
    screen_t * nat = make_screen(nw, nh, LV_COLOR_FORMAT_RGB565, rot, true, false);
    lv_display_set_default(nat->disp);
    lv_obj_t * scr = lv_display_get_screen_active(nat->disp);
    lv_obj_t * obj = scr;
    if(frame) {
        obj = lv_image_create(scr);
        lv_image_set_src(obj, frame);
    }
    else scene_ui(scr);
    lv_draw_ortho_reset_stats();
    redraw(nat->disp, obj, phase);
    const lv_draw_ortho_stats_t * st = lv_draw_ortho_get_stats();
    printf("ortho+ppa %d: %u of 2 tiles in turn, %.0f%% of the turning by the PPA, %u waits\n", phase,
           st->tiles_in_turn, st->bytes_turned ? 100.0 * st->bytes_handed / st->bytes_turned : 0.0, st->waits / 2);
    drop_screen(nat);
    lv_draw_ortho_ppa_deinit();
}

/* The UI of the PC bench (scene_ui), redrawn whole: upright as ESPHome's
 * lvgl, esp_lvgl_port and esp_lvgl_adapter draw it, then by lv_draw_ortho. */
static void ui(int32_t nw, int32_t nh, lv_display_rotation_t rot, int phase)
{
    printf("UI %dx%d\n", (int)nw, (int)nh);
    const lv_color_format_t cf = LV_COLOR_FORMAT_RGB565;
    const bool swap = rot == LV_DISPLAY_ROTATION_90 || rot == LV_DISPLAY_ROTATION_270;

    screen_t * up = make_screen(swap ? nh : nw, swap ? nw : nh, cf, LV_DISPLAY_ROTATION_0, false, false);
    lv_display_set_default(up->disp);
    lv_obj_t * s1 = lv_display_get_screen_active(up->disp);
    scene_ui(s1);
    redraw(up->disp, s1, phase);
    drop_screen(up);

    screen_t * nat = make_screen(nw, nh, cf, rot, true, false);
    lv_display_set_default(nat->disp);
    lv_obj_t * s2 = lv_display_get_screen_active(nat->disp);
    scene_ui(s2);
    lv_draw_ortho_reset_stats();
    redraw(nat->disp, s2, phase + 1);
    const lv_draw_ortho_stats_t * st = lv_draw_ortho_get_stats();
    /* Two redraws were counted: the warm one and the measured one. */
    printf("ortho %d: tile_internal %d, per redraw %llu KiB turned, %llu KiB straight, %u tiles\n", phase + 1,
           st->tile_internal, (unsigned long long)st->bytes_turned / 2048,
           (unsigned long long)st->bytes_straight / 2048, st->tiles / 2);
    drop_screen(nat);
    ortho_with_ppa(nw, nh, rot, phase + 2, NULL);
}

/* A full-screen picture (a video frame drawn by LVGL as an image). */
static void picture(int32_t nw, int32_t nh, lv_display_rotation_t rot, int phase)
{
    printf("picture %dx%d\n", (int)nw, (int)nh);
    const lv_color_format_t cf = LV_COLOR_FORMAT_RGB565;
    const bool swap = rot == LV_DISPLAY_ROTATION_90 || rot == LV_DISPLAY_ROTATION_270;
    lv_image_dsc_t * frame = image_of(swap ? nh : nw, swap ? nw : nh, cf, 3);

    screen_t * up = make_screen(swap ? nh : nw, swap ? nw : nh, cf, LV_DISPLAY_ROTATION_0, false, false);
    lv_display_set_default(up->disp);
    lv_obj_t * i1 = lv_image_create(lv_display_get_screen_active(up->disp));
    lv_image_set_src(i1, frame);
    redraw(up->disp, i1, phase);
    drop_screen(up);

    screen_t * nat = make_screen(nw, nh, cf, rot, true, false);
    lv_display_set_default(nat->disp);
    lv_obj_t * i2 = lv_image_create(lv_display_get_screen_active(nat->disp));
    lv_image_set_src(i2, frame);
    redraw(nat->disp, i2, phase + 1);
    drop_screen(nat);
    ortho_with_ppa(nw, nh, rot, phase + 2, frame);
}

/* The calibration: a memcpy of 1280x620 RGB565, PSRAM to PSRAM -- what the
 * Tab5 measured at 32 to 35 ms when portall copied a decoded picture into an
 * LVGL canvas (CLAUDE.md of esphome_esp-video, "Flashed, and the ceiling is
 * now a number"). */
static void calibrate(void)
{
    const size_t n = 1280 * 620 * 2;
    uint8_t * a = malloc(n), * b = malloc(n);
    memset(a, 1, n);
    memset(b, 2, n);
    memcpy(b, a, n);
    PHASE(10);
    memcpy(b, a, n);
    PHASE(1999);
    free(a);
    free(b);
}

int main(void)
{
    printf("starting LVGL\n");
    lv_init();
    lv_tick_set_cb(tick_ms);
    lv_draw_ortho_init();
    g_photo = image_of(97, 61, LV_COLOR_FORMAT_RGB565, 1);
    g_alpha = image_of(80, 50, LV_COLOR_FORMAT_ARGB8888, 2);

    printf("calibrating\n");
    calibrate();
    /* LVGL's rotation, as the PC bench names it. */
    ui(720, 1280, LV_DISPLAY_ROTATION_270, 20);   /* Tab5 */
    ui(800, 1280, LV_DISPLAY_ROTATION_90, 30);    /* Guition JC8012P4A1 */
    ui(1024, 600, LV_DISPLAY_ROTATION_180, 40);   /* Waveshare 7B */
    picture(720, 1280, LV_DISPLAY_ROTATION_270, 50);
    picture(800, 1280, LV_DISPLAY_ROTATION_90, 60);
    picture(1024, 600, LV_DISPLAY_ROTATION_180, 70);
    printf("done\n");
    return 0;
}
