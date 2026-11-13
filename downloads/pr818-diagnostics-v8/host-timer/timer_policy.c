/* SPDX-FileCopyrightText: 2026 youkorr
 * SPDX-License-Identifier: MIT
 * Real LVGL regression: no timer/display API is mocked.
 */
#include "lvgl.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lv_display_t *display;
static lv_obj_t *screen, *marker;
static bool strict, manual, track, inject_once;
static unsigned manual_starts, auto_starts, manual_ready, auto_ready;
static unsigned requests, requests_seen_resumed, manual_copies, auto_copies;
static unsigned manual_flushes, auto_flushes, heartbeat;
static size_t manual_copy_bytes, auto_copy_bytes;

void __real_lv_draw_buf_copy(lv_draw_buf_t *, const lv_area_t *, const lv_draw_buf_t *, const lv_area_t *);
void __wrap_lv_draw_buf_copy(lv_draw_buf_t *dest, const lv_area_t *dest_area,
                             const lv_draw_buf_t *src, const lv_area_t *src_area)
{
    if(track) {
        const lv_area_t *a = src_area ? src_area : dest_area;
        const size_t bytes = a ? (size_t)lv_area_get_width(a) * lv_area_get_height(a) * 2 :
                                (size_t)src->header.w * src->header.h * 2;
        if(manual) { manual_copies++; manual_copy_bytes += bytes; }
        else { auto_copies++; auto_copy_bytes += bytes; }
    }
    __real_lv_draw_buf_copy(dest, dest_area, src, src_area);
}

static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *pixels)
{
    (void)a; (void)pixels;
    if(track) { if(manual) manual_flushes++; else auto_flushes++; }
    lv_display_flush_ready(d);
}

static void beat(lv_timer_t *timer)
{
    (void)timer;
    heartbeat++;
}

static void observe(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if(code == LV_EVENT_REFR_REQUEST) {
        /* Default LVGL callback was registered first and has resumed timer. */
        if(track) {
            requests++;
            if(!lv_timer_get_paused(lv_display_get_refr_timer(display))) requests_seen_resumed++;
        }
        if(strict) lv_timer_pause(lv_display_get_refr_timer(display));
    }
    else if(code == LV_EVENT_REFR_START) {
        if(track) { if(manual) manual_starts++; else auto_starts++; }
        if(manual && inject_once) {
            inject_once = false;
            /* Legitimate invalidation during refresh. Existing dirty region
             * is only the marker, so this full-screen area is not redundant.
             * The current refresh consumes it before returning. */
            lv_obj_invalidate(screen);
        }
    }
    else if(code == LV_EVENT_REFR_READY && track) {
        if(manual) manual_ready++; else auto_ready++;
    }
}

static void refresh(void)
{
    manual = true;
    lv_refr_now(display);
    manual = false;
    if(strict) lv_timer_pause(lv_display_get_refr_timer(display));
}

static void reset_counters(void)
{
    manual_starts = auto_starts = manual_ready = auto_ready = requests = requests_seen_resumed = 0;
    manual_copies = auto_copies = manual_flushes = auto_flushes = heartbeat = 0;
    manual_copy_bytes = auto_copy_bytes = 0;
}

static void run_case(bool controlled)
{
    strict = controlled;
    track = false;
    lv_init();
    display = lv_display_create(320, 240);
    assert(display);
    static uint16_t buffer1[320 * 240], buffer2[320 * 240];
    memset(buffer1, 0, sizeof(buffer1)); memset(buffer2, 0, sizeof(buffer2));
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buffer1, buffer2, sizeof(buffer1), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(display, flush);
    lv_display_add_event_cb(display, observe, LV_EVENT_ALL, NULL);
    screen = lv_display_get_screen_active(display);
    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    marker = lv_obj_create(screen);
    lv_obj_remove_style_all(marker);
    lv_obj_set_pos(marker, 20, 20); lv_obj_set_size(marker, 32, 32);
    lv_obj_set_style_bg_opa(marker, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(marker, lv_color_hex(0x204080), 0);
    lv_timer_t *other = lv_timer_create(beat, 5, NULL);
    assert(other);
    lv_timer_pause(lv_display_get_refr_timer(display));
    refresh();
    /* Drain initial full-screen DIRECT synchronization outside tracking. */
    refresh();
    reset_counters();
    track = true;
    lv_obj_set_style_bg_color(marker, lv_color_hex(0x804020), 0);
    inject_once = true;
    refresh();
    const bool paused_after_manual = lv_timer_get_paused(lv_display_get_refr_timer(display));
    /* Run actual other LVGL timers. A later display refresh is automatic. */
    for(unsigned t = 0; t < 80; t++) { lv_tick_inc(1); lv_timer_handler(); }
    assert(manual_starts == 1 && manual_ready == 1);
    assert(requests_seen_resumed > 0); /* Confirms actual default-handler order. */
    assert(heartbeat >= 10); /* Strict policy must not freeze all LVGL timers. */
    if(controlled) {
        assert(paused_after_manual);
        assert(auto_starts == 0 && auto_ready == 0 && auto_copies == 0 && auto_flushes == 0);
    }
    else {
        assert(!paused_after_manual);
        assert(auto_starts == 1 && auto_ready == 1);
        assert(auto_copies > 0 && auto_copy_bytes == sizeof(buffer1));
        assert(auto_flushes == 0); /* Empty automatic refresh still copied. */
    }
    printf("{\"case\":\"%s\",\"manual_starts\":%u,\"automatic_starts\":%u,\"automatic_flushes\":%u,\"automatic_copy_calls\":%u,\"automatic_copy_pixel_bytes\":%zu,\"request_callbacks_after_default_resume\":%u,\"paused_after_manual\":%s,\"other_timer_callbacks\":%u}\n",
           controlled ? "strict" : "legacy", manual_starts, auto_starts, auto_flushes,
           auto_copies, auto_copy_bytes, requests_seen_resumed, paused_after_manual ? "true" : "false", heartbeat);
    track = false;
    lv_timer_delete(other); lv_display_delete(display); lv_deinit();
}

int main(void)
{
    run_case(false);
    run_case(true);
    puts("PASS: real LVGL legacy rearm creates empty DIRECT-sync copy; strict suppresses it and preserves other timers");
    return 0;
}
