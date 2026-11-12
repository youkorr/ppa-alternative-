/* SPDX-FileCopyrightText: 2026 youkorr
 * SPDX-License-Identifier: MIT
 * Hardware timing harness; included after the original dashboard benchmark.
 */
#include "board_bench_config.h"
#include "../../../src/display/rotate_in_draw/lv_draw_ortho.h"
#include "src/misc/cache/instance/lv_image_cache.h"

extern const uint8_t board_image_start[] asm("_binary_board_image_rgb565_start");

typedef struct {
    int64_t start, flush, last_start, max_gap, render_sum, render_worst;
    int64_t refresh_sum, refresh_worst;
    uint32_t frames;
} board_metrics_t;

static board_metrics_t s_metrics;

static void board_event(lv_event_t *event)
{
    const int64_t now = esp_timer_get_time();
    switch (lv_event_get_code(event)) {
    case LV_EVENT_RENDER_START:
        if (s_metrics.last_start && now - s_metrics.last_start > s_metrics.max_gap) {
            s_metrics.max_gap = now - s_metrics.last_start;
        }
        s_metrics.last_start = s_metrics.start = now;
        s_metrics.flush = 0;
        break;
    case LV_EVENT_FLUSH_START:
        s_metrics.flush = now;
        break;
    case LV_EVENT_REFR_READY:
        if (s_metrics.start) {
            const int64_t refresh = now - s_metrics.start;
            const int64_t render = s_metrics.flush > s_metrics.start ? s_metrics.flush - s_metrics.start : refresh;
            s_metrics.render_sum += render;
            s_metrics.refresh_sum += refresh;
            if (render > s_metrics.render_worst) {
                s_metrics.render_worst = render;
            }
            if (refresh > s_metrics.refresh_worst) {
                s_metrics.refresh_worst = refresh;
            }
            s_metrics.frames++;
            s_metrics.start = 0;
        }
        break;
    default:
        break;
    }
}

static void board_cache(size_t bytes)
{
#if BENCH_HAS_CACHE
    lv_draw_ortho_set_image_cache(bytes);
#else
    (void)bytes;
#endif
}

/* A full-size image changed between two PSRAM buffers. This exercises the
 * display path used for mutable frames; it does not capture from a sensor. */
static void board_phase(const char *scene, int round, size_t budget, bool moving, bool native, bool ui)
{
    const size_t bytes = (size_t)BOARD_WIDTH * BOARD_HEIGHT * 2;
    TEST_ESP_OK(esp_lv_adapter_lock(pdMS_TO_TICKS(10000)));
    board_cache(budget);
    lv_obj_t *scr = lv_display_get_screen_active(s_disp);
    lv_obj_clean(scr);
    lv_obj_set_layout(scr, LV_LAYOUT_NONE);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_NONE, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    uint8_t *buffers[2] = {NULL, NULL};
    if (moving) {
        for (int i = 0; i < 2; i++) {
            buffers[i] = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            TEST_ASSERT_NOT_NULL(buffers[i]);
            /* The source pixels are fixed during each refresh; only the next
             * refresh may change the descriptor. No sensor/ISP work is timed. */
            memcpy(buffers[i], board_image_start, bytes);
            if (i) for (size_t j = 0; j < bytes; j++) {
                    buffers[i][j] ^= 0x7F;
                }
        }
    }
    lv_image_dsc_t image = {
        .header = {
            .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
            .w = BOARD_WIDTH, .h = BOARD_HEIGHT,
            .stride = (native ? HW_LCD_H_RES : BOARD_WIDTH) * 2,
            .flags = moving ? LV_IMAGE_FLAGS_MODIFIABLE : 0
        },
        .data_size = bytes,
        .data = moving ? buffers[0] : board_image_start,
    };
    if (native) {
        image.header.flags |= LV_DRAW_ORTHO_IMAGE_FLAG_NATIVE;
    }
    lv_obj_t *obj = NULL;
    if (ui) {
        bench_build_ui(scr);
    } else {
        obj = lv_image_create(scr);
        lv_image_set_src(obj, &image);
        lv_obj_set_pos(obj, 0, 0);
    }
    const int64_t warm_started = esp_timer_get_time();
    for (int i = 0; i < 3; i++) {
        lv_obj_invalidate(scr);
        lv_refr_now(s_disp);
    }
    const int64_t warm_us = esp_timer_get_time() - warm_started;
    lv_draw_ortho_reset_stats();
    memset(&s_metrics, 0, sizeof(s_metrics));
    lv_display_add_event_cb(s_disp, board_event, LV_EVENT_ALL, NULL);
    esp_lv_adapter_unlock();

    int frame = 0;
    const int64_t started = esp_timer_get_time();
    while (esp_timer_get_time() - started < 8000000) {
        TEST_ESP_OK(esp_lv_adapter_lock(pdMS_TO_TICKS(10000)));
        if (moving) {
            lv_image_cache_drop(&image);
            image.data = buffers[(++frame) & 1];
            lv_image_set_src(obj, &image);
        }
        lv_obj_invalidate(scr);
        lv_refr_now(s_disp);
        esp_lv_adapter_unlock();
        vTaskDelay(1);
    }
    TEST_ESP_OK(esp_lv_adapter_lock(pdMS_TO_TICKS(10000)));
    const int64_t elapsed = esp_timer_get_time() - started;
    lv_display_remove_event_cb_with_user_data(s_disp, board_event, NULL);
    const board_metrics_t measured = s_metrics;
    const lv_draw_ortho_stats_t stats = *lv_draw_ortho_get_stats();
    uint32_t hits = 0, builds = 0, kept = 0;
    uint64_t straight_dma = 0;
#if BENCH_HAS_CACHE
    hits = stats.images_cached;
    builds = stats.image_cache_builds;
    kept = stats.image_cache_bytes;
    straight_dma = stats.bytes_straight_handed;
#endif
    lv_obj_clean(scr);
    lv_obj_set_layout(scr, LV_LAYOUT_NONE);
    if (ui) {
        lv_style_reset(&s_bench_card);
    }
    lv_image_cache_drop(&image);
    board_cache(0);
    for (int i = 0; i < 2; i++) {
        heap_caps_free(buffers[i]);
    }
    esp_lv_adapter_unlock();
    TEST_ASSERT_GREATER_THAN_UINT32(0, measured.frames);
    ESP_LOGI(TAG, "PR818_RESULT board=%s revision=%s scene=%s round=%d budget_kib=%u frames=%u elapsed_ms=%.3f "
             "fps=%.3f render_ms=%.3f worst_render_ms=%.3f refresh_ms=%.3f worst_refresh_ms=%.3f max_gap_ms=%.3f "
             "warmup_ms=%.3f cache_hits=%u cache_builds=%u cache_bytes=%u turned=%u native=%u other_tiles=%u failed=%u "
             "cores=%u turned_kib=%llu straight_kib=%llu handed_kib=%llu straight_dma_kib=%llu",
             BOARD_NAME, BOARD_REVISION, scene, round, (unsigned)(budget / 1024), (unsigned)measured.frames,
             elapsed / 1000.0, measured.frames * 1e6 / (double)elapsed,
             measured.render_sum / 1000.0 / measured.frames, measured.render_worst / 1000.0,
             measured.refresh_sum / 1000.0 / measured.frames, measured.refresh_worst / 1000.0,
             measured.max_gap / 1000.0, warm_us / 1000.0, (unsigned)hits, (unsigned)builds, (unsigned)kept,
             (unsigned)stats.images_turned, (unsigned)stats.images_native,
             (unsigned)stats.tiles_other_core, (unsigned)stats.turner_failed, (unsigned)stats.cores,
             (unsigned long long)(stats.bytes_turned / 1024), (unsigned long long)(stats.bytes_straight / 1024),
             (unsigned long long)(stats.bytes_handed / 1024), (unsigned long long)(straight_dma / 1024));
    TEST_ASSERT_FALSE_MESSAGE(stats.turner_failed, "PPA timeout: compare this run as a failure, not an FPS result");
#if BENCH_HAS_CACHE
    if (budget && !ui && !moving) {
        TEST_ASSERT_GREATER_THAN_UINT32(0, hits);
        TEST_ASSERT_GREATER_THAN_UINT32(0, kept);
        TEST_ASSERT_EQUAL_UINT32(0, stats.images_turned);
    }
    if (moving) {
        TEST_ASSERT_EQUAL_UINT32(0, hits);
    }
#endif
}

TEST_CASE("PR818 board comparison", "[boardbench]")
{
    ESP_LOGI(TAG, "PR818_START board=%s revision=%s commit=%s logical=%dx%d cpu_mhz=%d l2_kib=%d "
             "psram_mhz=%d sensor_capture=0", BOARD_NAME, BOARD_REVISION, BOARD_COMMIT,
             BOARD_WIDTH, BOARD_HEIGHT, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
             CONFIG_CACHE_L2_CACHE_SIZE / 1024, CONFIG_SPIRAM_SPEED);
    TEST_ASSERT_EQUAL(BOARD_WIDTH, lv_display_get_horizontal_resolution(s_disp));
    TEST_ASSERT_EQUAL(BOARD_HEIGHT, lv_display_get_vertical_resolution(s_disp));
    TEST_ASSERT_TRUE(lv_display_get_matrix_rotation(s_disp));
    for (int round = 1; round <= 3; round++) {
        board_phase("ui", round, 0, false, false, true);
        board_phase("fixed_off", round, 0, false, false, false);
#if BENCH_HAS_CACHE
        board_phase("fixed_on", round, 4 * 1024 * 1024, false, false, false);
#endif
        board_phase("moving_off", round, 0, true, false, false);
#if BENCH_HAS_CACHE
        board_phase("moving_on", round, 4 * 1024 * 1024, true, false, false);
#endif
        board_phase("moving_native", round, 0, true, true, false);
    }
}
