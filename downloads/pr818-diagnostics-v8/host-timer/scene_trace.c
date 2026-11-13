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
static lv_obj_t *screen;
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


#include "src/misc/cache/instance/lv_image_cache.h"
#include <inttypes.h>
#define AB_WIDTH 1280
#define AB_HEIGHT 720
#define AB_DASHBOARD_FULL 0
#define s_display display
static lv_obj_t *s_arc, *s_bar, *s_slider, *s_counter, *s_marker;
static lv_image_dsc_t s_image;
static uint8_t *s_bank[4];
static const uint8_t *ab_wallpaper_start;
static void disable_scroll(lv_obj_t *object)
{
#if LVGL_VERSION_MAJOR > 9 || LVGL_VERSION_MINOR >= 6
    lv_obj_set_scrollable(object, false);
#else
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
#endif
}

static lv_obj_t *card(lv_obj_t *parent, int x, int y, int w, int h, const char *title)
{
    lv_obj_t *object = lv_obj_create(parent);
    lv_obj_set_pos(object, x, y);
    lv_obj_set_size(object, w, h);
    disable_scroll(object);
    lv_obj_set_style_bg_color(object, lv_color_hex(0x203048), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(object, 15, 0);
    lv_obj_set_style_border_color(object, lv_color_hex(0x526078), 0);
    lv_obj_set_style_border_width(object, 2, 0);
    lv_obj_set_style_shadow_width(object, 18, 0);
    lv_obj_set_style_shadow_opa(object, LV_OPA_40, 0);
    lv_obj_t *label = lv_label_create(object);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 0, 0);
    return object;
}

static void build_scene(unsigned kind)
{
    lv_obj_t *screen = lv_display_get_screen_active(s_display);
    lv_obj_clean(screen);
    lv_obj_remove_style_all(screen);
    disable_scroll(screen);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x081220), 0);
    s_arc = s_bar = s_slider = NULL;
    if (kind == 0) {
        lv_obj_set_style_bg_grad_color(screen, lv_color_hex(0x203758), 0);
        lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);
        const int cw = (AB_WIDTH - 80) / 3, ch = AB_HEIGHT - 145;
        lv_obj_t *c = card(screen, 20, 80, cw, ch, "Temperature");
        s_arc = lv_arc_create(c);
        lv_obj_set_size(s_arc, cw - 60, ch - 70);
        lv_obj_center(s_arc);
        lv_arc_set_range(s_arc, 0, 100);
        c = card(screen, 40 + cw, 80, cw, ch, "Power");
        s_bar = lv_bar_create(c);
        lv_obj_set_size(s_bar, cw - 45, 45);
        lv_obj_center(s_bar);
        c = card(screen, 60 + 2 * cw, 80, cw, ch, "Lighting");
        s_slider = lv_slider_create(c);
        lv_obj_set_width(s_slider, cw - 55);
        lv_obj_center(s_slider);
    } else {
        s_image = (lv_image_dsc_t) {
            .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                       .w = AB_WIDTH, .h = AB_HEIGHT, .stride = AB_WIDTH * 2,
                       .flags = kind == 1 ? LV_IMAGE_FLAGS_MODIFIABLE : 0},
            .data_size = AB_WIDTH * AB_HEIGHT * 2,
            .data = kind == 1 ? s_bank[0] : ab_wallpaper_start
        };
        lv_obj_t *image = lv_image_create(screen);
        lv_image_set_src(image, &s_image);
        lv_obj_set_pos(image, 0, 0);
        lv_obj_set_user_data(screen, image);
    }
    s_counter = lv_label_create(screen);
    lv_obj_set_style_text_font(s_counter, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_counter, lv_color_white(), 0);
    lv_obj_set_style_bg_color(s_counter, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_opa(s_counter, LV_OPA_COVER, 0);
    lv_obj_set_pos(s_counter, 16, 15);
    s_marker = lv_obj_create(screen);
    lv_obj_remove_style_all(s_marker);
    lv_obj_set_pos(s_marker, AB_WIDTH - 64, 8);
    lv_obj_set_size(s_marker, 48, 48);
    lv_obj_set_style_bg_opa(s_marker, LV_OPA_COVER, 0);
}

static void update_scene(unsigned kind, uint32_t frame)
{
    /* Every variant sees the same ordered trace. No animation depends on
     * elapsed wall time or on the throughput of the tested renderer. */
    if (kind == 0) {
        const int value = (frame * 7) % 101;
        lv_arc_set_value(s_arc, value);
        lv_bar_set_value(s_bar, (frame * 11) % 101, LV_ANIM_OFF);
        lv_slider_set_value(s_slider, (frame * 13) % 101, LV_ANIM_OFF);
    } else {
        if (kind == 1) {
            lv_image_cache_drop(&s_image);
            s_image.data = s_bank[frame % 4];
        }
        lv_obj_t *image = lv_obj_get_user_data(lv_display_get_screen_active(s_display));
        lv_image_set_src(image, &s_image);
        lv_obj_invalidate(image); /* fixed scene explicitly redraws its image */
    }
    char text[80];
    snprintf(text, sizeof(text), "A/B trace %03" PRIu32, frame);
    lv_label_set_text(s_counter, text);
    lv_obj_set_style_bg_color(s_marker, lv_color_hex(((frame * 37) & 255) << 16 |
                                                   ((frame * 71) & 255) << 8 |
                                                   ((frame * 109) & 255)), 0);
#if AB_DASHBOARD_FULL
    if (kind == 0) lv_obj_invalidate(lv_display_get_screen_active(s_display));
#endif
}


static void scene_trace(bool controlled, unsigned kind)
{
    strict = controlled; track = false; inject_once = false;
    lv_init();
    display = lv_display_create(AB_WIDTH, AB_HEIGHT); assert(display);
    const size_t bytes = AB_WIDTH * AB_HEIGHT * 2;
    void *buf[3] = {calloc(1,bytes),calloc(1,bytes),calloc(1,bytes)};
    assert(buf[0] && buf[1] && buf[2]);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buf[0], buf[1], bytes, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_draw_buf_t third;
    assert(lv_draw_buf_init(&third, AB_WIDTH, AB_HEIGHT, LV_COLOR_FORMAT_RGB565, AB_WIDTH * 2, buf[2], bytes) == LV_RESULT_OK);
    lv_display_set_3rd_draw_buffer(display, &third);
    lv_display_set_flush_cb(display, flush);
    lv_display_add_event_cb(display, observe, LV_EVENT_ALL, NULL);
    screen = lv_display_get_screen_active(display);
    for(unsigned b = 0; b < 4; b++) {
        s_bank[b] = malloc(bytes); assert(s_bank[b]);
        /* Precompute V7 input banks; values cannot affect invalidation logic. */
        for(unsigned y = 0; y < AB_HEIGHT; y++) for(unsigned x = 0; x < AB_WIDTH; x++) {
            const unsigned r = (x * 31 / AB_WIDTH + b * 7) & 31;
            const unsigned g = (y * 63 / AB_HEIGHT + b * 11) & 63;
            const unsigned blue = ((x / 48) ^ (y / 48) ^ (b * 9)) & 31;
            const uint16_t pixel = (r << 11) | (g << 5) | blue;
            memcpy(s_bank[b] + ((size_t)y * AB_WIDTH + x) * 2, &pixel, 2);
        }
    }
    ab_wallpaper_start = s_bank[0];
    lv_timer_t *other = lv_timer_create(beat, 5, NULL);
    assert(other);
    lv_timer_pause(lv_display_get_refr_timer(display));
    build_scene(kind);
    for(unsigned i = 0; i < 16; i++) {
        update_scene(kind,i); refresh();
        lv_tick_inc(40); lv_timer_handler();
    }
    reset_counters(); track = true;
    unsigned resumed_after_manual = 0;
    for(unsigned i = 0; i < 64; i++) {
        update_scene(kind,16+i); refresh();
        if(!lv_timer_get_paused(lv_display_get_refr_timer(display))) resumed_after_manual++;
        /* Controlled opportunity for the background timer after unlocking.
         * This is NOT wall-clock or RTOS scheduling emulation. */
        lv_tick_inc(40); lv_timer_handler();
    }
    assert(manual_starts == 64 && manual_ready == 64);
    assert(heartbeat == 64);
    if(controlled) assert(auto_starts == 0 && auto_copies == 0 && auto_flushes == 0);
    printf("{\"case\":\"v7_scene_trace\",\"mode\":\"%s\",\"scene\":%u,\"frames\":64,\"resumed_after_manual\":%u,\"automatic_starts\":%u,\"automatic_flushes\":%u,\"automatic_copy_calls\":%u,\"automatic_copy_pixel_bytes\":%zu,\"manual_copy_calls\":%u,\"manual_copy_pixel_bytes\":%zu,\"other_timer_callbacks\":%u}\n",
           controlled ? "strict" : "legacy", kind, resumed_after_manual,auto_starts,auto_flushes,
           auto_copies,auto_copy_bytes,manual_copies,manual_copy_bytes,heartbeat);
    track=false;
    lv_timer_delete(other);lv_obj_clean(screen);lv_image_cache_drop(&s_image);
    lv_display_delete(display);lv_deinit();
    for(unsigned b = 0; b < 4; b++) free(s_bank[b]);
    for(unsigned b = 0; b < 3; b++) free(buf[b]);
}
int main(void)
{
    for(unsigned kind=0;kind<3;kind++) {scene_trace(false,kind);scene_trace(true,kind);}
    return 0;
}
