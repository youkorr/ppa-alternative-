/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 youkorr
 * Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
 *
 * A Waveshare 7B (1024x600 landscape) shown in portrait, drawn by
 * lv_draw_ortho: LVGL draws in the panel's own orientation, the PPA turns
 * the tiles into the panel's frame buffers, and nothing turns the frame
 * afterwards. The two frame buffers are the panel's own (DIRECT mode); a
 * frame is shown by switching to it, so nothing is copied to the panel
 * either.
 */
#include <inttypes.h>

#include "esp_err.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "example_lcd.h"
#include "lv_draw_ortho.h"
#include "lv_draw_ortho_ppa.h"

#define ROTATION LV_DISPLAY_ROTATION_90
#define FRAME_BUFFERS 2

static const char *TAG = "example";
static esp_lcd_panel_handle_t s_panel;
static lv_display_t *s_disp;
static volatile bool s_showing;

static uint32_t tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* The panel has switched to the frame LVGL handed over: the one it showed
 * before is free for the next frame. */
static bool on_frame_buf_complete(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata, void *ctx)
{
    (void)panel;
    (void)edata;
    (void)ctx;
    if (s_showing) {
        s_showing = false;
        lv_display_flush_ready(s_disp);
    }
    return false;
}

/* DIRECT mode into the panel's own buffers: every area is already where it
 * belongs, and the last one of a frame shows it. */
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)area;
    if (!lv_display_flush_is_last(disp)) {
        lv_display_flush_ready(disp);
        return;
    }
    s_showing = true;
    if (esp_lcd_panel_draw_bitmap(s_panel, 0, 0, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES, px_map) != ESP_OK) {
        s_showing = false;
        lv_display_flush_ready(disp);
    }
}

static void report_cb(lv_timer_t *timer)
{
    (void)timer;
    static uint32_t last_tiles;
    const lv_draw_ortho_stats_t *st = lv_draw_ortho_get_stats();
    ESP_LOGI(TAG, "%" PRIu32 " tiles in 5 s (%" PRIu32 " on the second core), %llu KiB turned by the PPA, "
             "%" PRIu32 " fills by the PPA", st->tiles - last_tiles, st->tiles_other_core,
             (unsigned long long)(st->bytes_handed / 1024), st->fills_handed);
    last_tiles = st->tiles;
}

static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x10151c), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(0x24425f), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "lv_draw_ortho");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t *spinner = lv_spinner_create(scr);
    lv_obj_set_size(spinner, 200, 200);
    lv_obj_center(spinner);

    lv_obj_t *note = lv_label_create(scr);
    lv_label_set_text(note, "Drawn in the panel's orientation,\nturned by the PPA tile by tile.");
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(note, lv_color_hex(0xc8d4e0), 0);
    lv_obj_align(note, LV_ALIGN_BOTTOM_MID, 0, -60);

    lv_timer_create(report_cb, 5000, NULL);
}

static void lvgl_task(void *arg)
{
    (void)arg;
    lv_init();
    lv_tick_set_cb(tick_ms);

    ESP_ERROR_CHECK(example_lcd_init(FRAME_BUFFERS, &s_panel));
    const esp_lcd_dpi_panel_event_callbacks_t cbs = {.on_frame_buf_complete = on_frame_buf_complete};
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_register_event_callbacks(s_panel, &cbs, NULL));
    void *fb[FRAME_BUFFERS];
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(s_panel, FRAME_BUFFERS, &fb[0], &fb[1]));

    /* The display keeps the panel's size; LVGL swaps the logical resolution. */
    s_disp = lv_display_create(EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, fb[0], fb[1], EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES * 2,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(s_disp, flush_cb);

    /* lv_draw_ortho: the draw unit, the PPA as its turner, the first tile lent
     * as the bands a big PSRAM picture is turned through, LVGL's copies
     * between the two frame buffers, and the second core. */
    lv_draw_ortho_init();
    if (lv_draw_ortho_ppa_init() == ESP_OK) {
        lv_draw_ortho_ppa_use_tiles(true);
    } else {
        ESP_LOGW(TAG, "no PPA client: the CPU turns the tiles");
    }
    lv_display_set_rotation(s_disp, ROTATION);
    lv_display_set_matrix_rotation(s_disp, true);
    if (!lv_draw_ortho_take_sync(s_disp)) {
        ESP_LOGW(TAG, "LVGL keeps the frame buffers in step itself");
    }
    if (!lv_draw_ortho_set_cores(2)) {
        ESP_LOGW(TAG, "one core paints the tiles");
    }

    build_ui();
    for (;;) {
        const uint32_t ms = lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(LV_CLAMP(1, ms, 30)));
    }
}

void app_main(void)
{
    /* LVGL and the engine on core 0; the second core paints every other tile. */
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 32 * 1024, NULL, 4, NULL, 0);
}
