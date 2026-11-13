/* SPDX-FileCopyrightText: 2026 youkorr
 * SPDX-License-Identifier: MIT
 * Identical application workload for the official/control/native variants.
 */
#include "ab_bench_config.h"
#include "ab_scanout_probe.h"
#include "hw_init.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lv_adapter.h"
#include "esp_lv_adapter_display.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if AB_HAS_NATIVE
#include "../../../src/display/rotate_in_draw/lv_draw_ortho.h"
#endif

static const char *TAG = "ab_bench";
extern const uint8_t ab_wallpaper_start[] asm("_binary_ab_wallpaper_rgb565_start");
extern const uint8_t ab_wallpaper_end[] asm("_binary_ab_wallpaper_rgb565_end");
static lv_display_t *s_display;
static lv_obj_t *s_arc, *s_bar, *s_slider, *s_counter, *s_marker;
static lv_image_dsc_t s_image;
static uint8_t *s_bank[4];

static void disable_scroll(lv_obj_t *object)
{
#if LVGL_VERSION_MAJOR > 9 || LVGL_VERSION_MINOR >= 6
    lv_obj_set_scrollable(object, false);
#else
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
#endif
}

static uint32_t percentile(uint32_t *values, unsigned count, unsigned percent)
{
    for (unsigned i = 1; i < count; i++) {
        const uint32_t value = values[i];
        unsigned j = i;
        while (j && values[j - 1] > value) { values[j] = values[j - 1]; j--; }
        values[j] = value;
    }
    return values[((count - 1) * percent + 99) / 100];
}

static void cache_budget(size_t bytes)
{
#if AB_HAS_NATIVE
    lv_draw_ortho_set_image_cache(bytes);
#else
    assert(bytes == 0);
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

static void prepare_bank(void)
{
    const size_t bytes = (size_t)AB_WIDTH * AB_HEIGHT * 2;
    for (unsigned bank = 0; bank < 4; bank++) {
        s_bank[bank] = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        assert(s_bank[bank]);
        /* Sensor/ISP work, source generation and allocation are deliberately
         * outside the measured interval. These are synthetic changing frames. */
        for (unsigned y = 0; y < AB_HEIGHT; y++) for (unsigned x = 0; x < AB_WIDTH; x++) {
            const unsigned r = (x * 31 / AB_WIDTH + bank * 7) & 31;
            const unsigned g = (y * 63 / AB_HEIGHT + bank * 11) & 63;
            const unsigned b = ((x / 48) ^ (y / 48) ^ (bank * 9)) & 31;
            const uint16_t pixel = (r << 11) | (g << 5) | b;
            memcpy(s_bank[bank] + ((size_t)y * AB_WIDTH + x) * 2, &pixel, 2);
        }
    }
}

static void phase(const char *scene, unsigned kind, unsigned round, size_t cache_bytes)
{
    uint32_t *refresh_us = heap_caps_malloc(AB_FRAMES * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(refresh_us);
    ESP_ERROR_CHECK(esp_lv_adapter_lock(10000));
    cache_budget(cache_bytes);
    build_scene(kind);
    ab_probe_begin();
    for (unsigned i = 0; i < AB_WARMUP; i++) {
        update_scene(kind, i);
        lv_refr_now(s_display);
        if ((i & 15) == 15) { esp_lv_adapter_unlock(); vTaskDelay(1); ESP_ERROR_CHECK(esp_lv_adapter_lock(10000)); }
    }
    esp_lv_adapter_unlock();
    vTaskDelay(pdMS_TO_TICKS(80)); /* Drain old warm-up submissions outside measurement. */
    const ab_probe_stats_t warm = ab_probe_snapshot();
    ab_probe_end();
#if AB_HAS_DPI_PROBE
    assert(warm.scanout_irqs > 0); /* Reject a missing/unlinked private driver probe. */
#else
    (void)warm;
#endif
    const size_t free_before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t largest_internal_before = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint64_t refresh_sum = 0;
    unsigned idle_yields = 0;
#if AB_HAS_NATIVE
    ESP_ERROR_CHECK(esp_lv_adapter_lock(10000));
    lv_draw_ortho_reset_stats();
    esp_lv_adapter_unlock();
#endif
    ab_probe_begin();
    ESP_ERROR_CHECK(heap_caps_monitor_local_minimum_free_size_start());
    const int64_t started = esp_timer_get_time();
    for (uint32_t i = 0; i < AB_FRAMES; i++) {
        ESP_ERROR_CHECK(esp_lv_adapter_lock(10000));
        update_scene(kind, AB_WARMUP + i);
        ab_probe_frame(i + 1);
        const int64_t draw_start = esp_timer_get_time();
        lv_refr_now(s_display);
        refresh_us[i] = (uint32_t)(esp_timer_get_time() - draw_start);
        refresh_sum += refresh_us[i];
        esp_lv_adapter_unlock();
        if ((i & 15) == 15) { vTaskDelay(1); idle_yields++; }
    }
    const int64_t produced_us = esp_timer_get_time() - started;
    /* A bounded drain includes the final submitted generation in scanout
     * throughput and exposes skipped/superseded generations honestly. */
    const int64_t deadline = esp_timer_get_time() + 500000;
    ab_probe_stats_t probe;
    do {
        probe = ab_probe_snapshot();
        if (!AB_HAS_DPI_PROBE || probe.last_selected == AB_FRAMES) break;
        vTaskDelay(1);
    } while (esp_timer_get_time() < deadline);
    ab_probe_end();
    const size_t minimum_free = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    const size_t minimum_internal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(heap_caps_monitor_local_minimum_free_size_stop());
    const bool heap_ok = heap_caps_check_integrity_all(true);
    const uint32_t median = percentile(refresh_us, AB_FRAMES, 50);
    const uint32_t p95 = percentile(refresh_us, AB_FRAMES, 95);
    const uint32_t worst = refresh_us[AB_FRAMES - 1];
    uint32_t cache_hits = 0, cache_kept = 0, turner_failed = 0, cores = 0;
    uint32_t tile_internal = 0, tiles_in_turn = 0, batches = 0, waits = 0;
    uint64_t bytes_turned = 0;
#if AB_HAS_NATIVE
    ESP_ERROR_CHECK(esp_lv_adapter_lock(10000));
    const lv_draw_ortho_stats_t stats = *lv_draw_ortho_get_stats();
    cache_hits = stats.images_cached; cache_kept = stats.image_cache_bytes;
    turner_failed = stats.turner_failed; cores = stats.cores;
    tile_internal = stats.tile_internal; tiles_in_turn = stats.tiles_in_turn;
    batches = stats.batches; waits = stats.waits; bytes_turned = stats.bytes_turned;
    esp_lv_adapter_unlock();
    if (cache_bytes) assert(cache_hits && cache_kept);
    if (kind == 1) assert(cache_hits == 0);
#endif
    const bool observed = AB_HAS_DPI_PROBE && probe.scanout_irqs && !probe.invalid_indices &&
                          !probe.failures && probe.submissions == AB_FRAMES && probe.last_selected == AB_FRAMES;
    const int64_t cohort_us = observed ? probe.last_selected_us - started : 0;
    ESP_LOGI(TAG, "AB_RESULT board=%s variant=%s batch=%u scene=%s round=%u frames=%u "
             "produced_us=%lld submitted_fps=%.4f refresh_mean_us=%.2f refresh_median_us=%u refresh_p95_us=%u refresh_worst_us=%u "
             "scanout_metric=%s selected=%u unselected=%u scanout_cohort_us=%lld scanout_fps=%.4f "
             "draw_calls=%u scanout_irqs=%u invalid_indices=%u failures=%u "
             "cache_budget=%u cache_hits=%u cache_bytes=%u cores=%u turner_failed=%u "
             "native_stats=%u tile_internal=%u tiles_in_turn=%u batches=%u waits=%u bytes_turned=%llu "
             "free_before=%u internal_before=%u free_after=%u internal_after=%u minimum_free=%u minimum_internal=%u "
             "largest_internal_before=%u largest_internal_after=%u heap_ok=%u idle_yields=%u tick_hz=%u",
             AB_BOARD_NAME, AB_VARIANT, AB_BATCH_LIMIT, scene, round, AB_FRAMES,
             (long long)produced_us, AB_FRAMES * 1e6 / produced_us, refresh_sum / (double)AB_FRAMES,
             median, p95, worst, observed ? "GENERATION_SELECTED_FOR_DMA" : "UNAVAILABLE",
             probe.selected, probe.submissions >= probe.selected ? probe.submissions - probe.selected : 0,
             (long long)cohort_us, cohort_us > 0 ? probe.selected * 1e6 / cohort_us : -1.0,
             probe.draw_calls, probe.scanout_irqs, probe.invalid_indices, probe.failures,
             (unsigned)cache_bytes, cache_hits, cache_kept, cores, turner_failed,
             AB_HAS_NATIVE, tile_internal, tiles_in_turn, batches, waits, (unsigned long long)bytes_turned,
             (unsigned)free_before, (unsigned)internal_before,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)minimum_free, (unsigned)minimum_internal, (unsigned)largest_internal_before,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_ok, idle_yields, CONFIG_FREERTOS_HZ);
    assert(heap_ok && !turner_failed && !probe.failures);
#if AB_HAS_DPI_PROBE
    assert(observed); /* Invalid probe capture is never a successful FPS result. */
#endif
    heap_caps_free(refresh_us);
    ESP_ERROR_CHECK(esp_lv_adapter_lock(10000));
    lv_obj_clean(lv_display_get_screen_active(s_display));
    lv_image_cache_drop(&s_image);
    cache_budget(0);
    esp_lv_adapter_unlock();
}

void app_main(void)
{
    ESP_LOGI(TAG, "AB_READY board=%s variant=%s source_commit=%s source_tree=%s "
             "logical=%ux%u rotation=%u panel_buffers=3 cpu_mhz=%u psram_mhz=%u l2_kib=%u "
             "batch=%u rounds=%u warmup=%u frames=%u dpi_probe=%u dashboard_full=%u sensor_capture=0 "
             "draw_units=%u ppa_draw_requested=%u",
             AB_BOARD_NAME, AB_VARIANT, AB_SOURCE_COMMIT, AB_SOURCE_TREE,
             AB_WIDTH, AB_HEIGHT, (unsigned)AB_ROTATION,
             CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, CONFIG_SPIRAM_SPEED, CONFIG_CACHE_L2_CACHE_SIZE / 1024,
             AB_BATCH_LIMIT, AB_ROUNDS, AB_WARMUP, AB_FRAMES, AB_HAS_DPI_PROBE, AB_DASHBOARD_FULL,
             CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT, AB_STOCK_PPA);
    assert(ab_wallpaper_end - ab_wallpaper_start == AB_WIDTH * AB_HEIGHT * 2);
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t io = NULL;
    assert(esp_lv_adapter_get_required_frame_buffer_count(AB_TEAR_MODE, AB_ROTATION) == 3);
    ESP_ERROR_CHECK(hw_lcd_init(&panel, &io, AB_TEAR_MODE, AB_ROTATION));
    void *buffers[3] = {0};
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(panel, 3, &buffers[0], &buffers[1], &buffers[2]));
    ab_probe_bind(panel, buffers, (size_t)HW_LCD_H_RES * HW_LCD_V_RES * 2);
    const esp_lv_adapter_config_t cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(esp_lv_adapter_init(&cfg));
    esp_lv_adapter_display_config_t display = ESP_LV_ADAPTER_DISPLAY_MIPI_DEFAULT_CONFIG(
        panel, io, HW_LCD_H_RES, HW_LCD_V_RES, AB_ROTATION);
    display.tear_avoid_mode = AB_TEAR_MODE;
    display.profile.enable_ppa_accel = AB_STOCK_PPA;
    s_display = esp_lv_adapter_register_display(&display);
    assert(s_display);
    assert(lv_display_get_horizontal_resolution(s_display) == AB_WIDTH);
    assert(lv_display_get_vertical_resolution(s_display) == AB_HEIGHT);
    ESP_ERROR_CHECK(esp_lv_adapter_start());
    /* The workload owns every refresh. Pause only the display refresh timer;
     * LVGL's other timers (including the adapter's async core/cache setup) run. */
    ESP_ERROR_CHECK(esp_lv_adapter_lock(10000));
    lv_timer_pause(lv_display_get_refr_timer(s_display));
    esp_lv_adapter_unlock();
    prepare_bank();
    for (unsigned round = 1; round <= AB_ROUNDS; round++) {
        phase("dashboard", 0, round, 0);
        phase("changing_rgb565", 1, round, 0);
        phase("fixed_flash_off", 2, round, 0);
#if AB_HAS_NATIVE
        phase("fixed_flash_warm_cache", 2, round, 4 * 1024 * 1024);
#endif
    }
    for (unsigned i = 0; i < 4; i++) heap_caps_free(s_bank[i]);
    ESP_LOGI(TAG, "AB_DONE board=%s variant=%s rounds=%u hardware_measurement=COMPLETE compare_only_matching_board_and_scene=1",
             AB_BOARD_NAME, AB_VARIANT, AB_ROUNDS);
    /* A single display init avoids conflating stock teardown behavior with
     * the rendering benchmark. V6 validates lifecycle separately. */
    while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}
