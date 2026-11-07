// SPDX-License-Identifier: MIT
// Copyright (c) 2026 youkorr
// Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
// Inserted into esp_lvgl_adapter's test app by prepare.py, from the line below on.

/* ---------------------------------------------------------------------------
 * Added by ppa-alternative's espressif/waveshare-7b/prepare.py; not part of
 * esp_lvgl_adapter. The UI of ppa-alternative's yaml/waveshare-test-lvgl.yaml
 * (text, cards with borders and shadows, a gradient, an arc, a bar, a slider,
 * a switch, buttons), redrawn whole for 10 s and timed from LV_EVENT_RENDER_START
 * to LV_EVENT_REFR_READY, as lvgl_bench does under ESPHome.
 * ------------------------------------------------------------------------- */

#if CONFIG_ESP_LVGL_ADAPTER_ROTATE_IN_DRAW
#define BENCH_MODE "ROTATE_IN_DRAW (DOUBLE_DIRECT, drawn in the panel's orientation)"
#else
#define BENCH_MODE "rotated copy (DEFAULT_MIPI_DSI tear mode)"
#endif

#if LV_FONT_MONTSERRAT_28
#define BENCH_FONT_TITLE (&lv_font_montserrat_28)
#else
#define BENCH_FONT_TITLE LV_FONT_DEFAULT
#endif
#if LV_FONT_MONTSERRAT_20
#define BENCH_FONT_CARD (&lv_font_montserrat_20)
#else
#define BENCH_FONT_CARD LV_FONT_DEFAULT
#endif

#define BENCH_TIME_MS (10000)

static int64_t s_bench_t0;
static int64_t s_bench_sum_us;
static int64_t s_bench_worst_us;
static uint32_t s_bench_frames;
static lv_style_t s_bench_card;

static void bench_render_start(lv_event_t *e)
{
    (void)e;
    s_bench_t0 = esp_timer_get_time();
}

static void bench_refr_ready(lv_event_t *e)
{
    (void)e;
    if (s_bench_t0 == 0) {
        return;
    }
    const int64_t d = esp_timer_get_time() - s_bench_t0;
    s_bench_t0 = 0;
    s_bench_sum_us += d;
    if (d > s_bench_worst_us) {
        s_bench_worst_us = d;
    }
    s_bench_frames++;
}

static lv_obj_t *bench_card(lv_obj_t *parent, int32_t w_pct, int32_t h_pct, const char *title)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_add_style(c, &s_bench_card, 0);
    lv_obj_set_size(c, lv_pct(w_pct), lv_pct(h_pct));
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    if (title) {
        lv_obj_t *l = lv_label_create(c);
        lv_label_set_text(l, title);
        lv_obj_set_style_text_color(l, lv_color_hex(0xDDE6F5), 0);
        lv_obj_set_style_text_font(l, BENCH_FONT_CARD, 0);
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 0);
    }
    return c;
}

static void bench_build_ui(lv_obj_t *scr)
{
    lv_style_init(&s_bench_card);
    lv_style_set_bg_color(&s_bench_card, lv_color_hex(0x1E2633));
    lv_style_set_bg_opa(&s_bench_card, LV_OPA_COVER);
    lv_style_set_border_color(&s_bench_card, lv_color_hex(0x3A4A63));
    lv_style_set_border_width(&s_bench_card, 2);
    lv_style_set_radius(&s_bench_card, 16);
    lv_style_set_shadow_color(&s_bench_card, lv_color_hex(0x000000));
    lv_style_set_shadow_width(&s_bench_card, 24);
    lv_style_set_shadow_opa(&s_bench_card, LV_OPA_60);
    lv_style_set_shadow_offset_y(&s_bench_card, 6);
    lv_style_set_pad_all(&s_bench_card, 12);

    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0B1220), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(0x23345A), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 16, 0);
    lv_obj_set_style_pad_row(scr, 16, 0);
    lv_obj_set_style_pad_column(scr, 16, 0);
    lv_obj_set_layout(scr, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(scr);
    lv_obj_set_width(title, lv_pct(100));
    lv_label_set_text(title, "esp_lvgl_adapter -- Waveshare 7B");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, BENCH_FONT_TITLE, 0);

    lv_obj_t *c = bench_card(scr, 30, 34, "Salon");
    lv_obj_t *arc = lv_arc_create(c);
    lv_obj_set_size(arc, lv_pct(60), lv_pct(75));
    lv_obj_align(arc, LV_ALIGN_CENTER, 0, 0);
    lv_arc_set_value(arc, 65);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x334155), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0xF59E0B), LV_PART_INDICATOR);
    lv_obj_t *t = lv_label_create(c);
    lv_label_set_text(t, "21.5 C");
    lv_obj_set_style_text_color(t, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, 0);

    c = bench_card(scr, 30, 34, "Lumiere");
    lv_obj_t *sw = lv_switch_create(c);
    lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_align(sw, LV_ALIGN_CENTER, 0, 0);
    lv_obj_t *sl = lv_slider_create(c);
    lv_obj_set_width(sl, lv_pct(85));
    lv_slider_set_value(sl, 40, LV_ANIM_OFF);
    lv_obj_align(sl, LV_ALIGN_BOTTOM_MID, 0, 0);

    c = bench_card(scr, 30, 34, "Batterie");
    lv_obj_t *bar = lv_bar_create(c);
    lv_obj_set_size(bar, lv_pct(85), lv_pct(18));
    lv_bar_set_value(bar, 72, LV_ANIM_OFF);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, 0);
    t = lv_label_create(c);
    lv_label_set_text(t, "72 %, 1.4 kW");
    lv_obj_set_style_text_color(t, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, 0);

    static const char *const names[] = { "Scene soir", "Volets", "Musique" };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = lv_button_create(scr);
        lv_obj_add_style(b, &s_bench_card, 0);
        lv_obj_set_size(b, lv_pct(22), lv_pct(16));
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, names[i]);
        lv_obj_center(l);
    }

    lv_obj_t *text = lv_label_create(scr);
    lv_obj_set_width(text, lv_pct(100));
    lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
    lv_label_set_text(text, "Le texte compte aussi : beaucoup de petites lettres, sur un fond en degrade, "
                      "comme un vrai tableau de bord.");
    lv_obj_set_style_text_color(text, lv_color_hex(0xB8C4D9), 0);
}

TEST_CASE("rotate bench: whole screen redrawn", "[bench]")
{
    ESP_LOGI(TAG, "BENCH start: %s, rotation 90", BENCH_MODE);

    TEST_ESP_OK(esp_lv_adapter_lock(pdMS_TO_TICKS(LVGL_LOCK_TIMEOUT_MS)));
    lv_obj_t *scr = lv_display_get_screen_active(s_disp);
    bench_build_ui(scr);
    lv_display_add_event_cb(s_disp, bench_render_start, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(s_disp, bench_refr_ready, LV_EVENT_REFR_READY, NULL);
    esp_lv_adapter_unlock();

    /* The first frame lays the UI out; leave it out of the figure. */
    vTaskDelay(pdMS_TO_TICKS(1000));
    TEST_ESP_OK(esp_lv_adapter_lock(pdMS_TO_TICKS(LVGL_LOCK_TIMEOUT_MS)));
    s_bench_t0 = 0;
    s_bench_sum_us = 0;
    s_bench_worst_us = 0;
    s_bench_frames = 0;
    esp_lv_adapter_unlock();

    const int64_t started = esp_timer_get_time();
    while (esp_timer_get_time() - started < (int64_t)BENCH_TIME_MS * 1000) {
        TEST_ESP_OK(esp_lv_adapter_lock(pdMS_TO_TICKS(LVGL_LOCK_TIMEOUT_MS)));
        lv_obj_invalidate(scr);
        esp_lv_adapter_unlock();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    const int64_t elapsed = esp_timer_get_time() - started;

    TEST_ESP_OK(esp_lv_adapter_lock(pdMS_TO_TICKS(LVGL_LOCK_TIMEOUT_MS)));
    const uint32_t frames = s_bench_frames;
    const int64_t sum = s_bench_sum_us, worst = s_bench_worst_us;
    lv_display_remove_event_cb_with_user_data(s_disp, bench_render_start, NULL);
    lv_display_remove_event_cb_with_user_data(s_disp, bench_refr_ready, NULL);
    lv_obj_clean(scr);
    lv_obj_set_layout(scr, LV_LAYOUT_NONE);
    esp_lv_adapter_unlock();
    lv_style_reset(&s_bench_card);

    TEST_ASSERT_GREATER_THAN_UINT32(0, frames);
    ESP_LOGI(TAG, "BENCH %s: %u frames in %.1f s (%.1f/s), %.1f ms each, worst %.1f ms",
             BENCH_MODE, (unsigned)frames, elapsed / 1e6, frames * 1e6 / (double)elapsed,
             sum / 1000.0 / frames, worst / 1000.0);
}

