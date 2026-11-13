/* SPDX-FileCopyrightText: 2026 youkorr
 * SPDX-License-Identifier: MIT
 * Identical application workload for the official/control/native variants.
 */
#include "ab_bench_config.h"
#include "ab_scanout_probe.h"
#include "ab_cost_probe.h"
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


/* All timer-policy state and refresh-event counters are owned by the adapter's
 * LVGL lock. They are neither ISR data nor read without that lock. */
typedef struct {
    uint32_t manual_starts, manual_ready, automatic_starts, automatic_ready;
    uint32_t requests, requests_running, strict_pauses, nested, unmatched_ready;
    uint64_t manual_us, automatic_us;
} ab_refresh_stats_t;
static ab_refresh_stats_t s_refresh_stats;
static bool s_strict_timer, s_manual_owned, s_refresh_tracking;
static bool s_refresh_in_progress, s_refresh_is_manual;
static int64_t s_refresh_started_us;

static void refresh_event(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_REFR_REQUEST) {
        lv_timer_t *timer = lv_display_get_refr_timer(s_display);
        if (s_refresh_tracking) {
            s_refresh_stats.requests++;
            if (!lv_timer_get_paused(timer)) s_refresh_stats.requests_running++;
        }
        /* LVGL's default callback was registered first and has just resumed
         * this timer. Leave its other timers and async setup running. */
        if (s_strict_timer) {
            lv_timer_pause(timer);
            if (s_refresh_tracking) s_refresh_stats.strict_pauses++;
        }
    } else if (code == LV_EVENT_REFR_START) {
        if (s_refresh_in_progress && s_refresh_tracking) s_refresh_stats.nested++;
        s_refresh_in_progress = true;
        s_refresh_is_manual = s_manual_owned;
        s_refresh_started_us = esp_timer_get_time();
        if (!s_refresh_is_manual) ab_cost_probe_set_background(true);
        if (s_refresh_tracking) {
            if (s_refresh_is_manual) s_refresh_stats.manual_starts++;
            else s_refresh_stats.automatic_starts++;
        }
    } else if (code == LV_EVENT_REFR_READY) {
        const uint64_t elapsed = (uint64_t)(esp_timer_get_time() - s_refresh_started_us);
        if (!s_refresh_in_progress && s_refresh_tracking) s_refresh_stats.unmatched_ready++;
        if (s_refresh_tracking && s_refresh_in_progress) {
            if (s_refresh_is_manual) {
                s_refresh_stats.manual_ready++;
                s_refresh_stats.manual_us += elapsed;
            } else {
                s_refresh_stats.automatic_ready++;
                s_refresh_stats.automatic_us += elapsed;
            }
        }
        if (!s_refresh_is_manual) ab_cost_probe_set_background(false);
        s_refresh_in_progress = false;
    }
}

static void manual_refresh(void)
{
    assert(!s_manual_owned);
    s_manual_owned = true;
    lv_refr_now(s_display);
    s_manual_owned = false;
}

static void enforce_timer_policy(void)
{
    if (s_strict_timer) lv_timer_pause(lv_display_get_refr_timer(s_display));
}

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

static void log_costs(const char *scene, const char *timer_mode, unsigned round,
                      const ab_cost_stats_t *costs)
{
    for (unsigned stage = 0; stage < AB_COST_STAGE_COUNT; stage++) {
        for (unsigned kind = 0; kind < AB_COST_KIND_COUNT; kind++) {
            const ab_cost_cell_t *cell = &costs->cell[stage][kind];
            if (!cell->calls) continue;
            ESP_LOGI(TAG, "AB_COST protocol=8 board=%s variant=%s timer_mode=%s scene=%s round=%u "
                     "stage=%s kind=%s calls=%u us=%llu bytes=%llu pixels=%llu errors=%u pending=%u "
                     "flags_or=%u c2m_bytes=%llu m2c_bytes=%llu inclusive=1",
                     AB_BOARD_NAME, AB_VARIANT, timer_mode, scene, round,
                     ab_cost_probe_stage_name((ab_cost_stage_t)stage),
                     ab_cost_probe_kind_name((ab_cost_kind_t)kind),
                     cell->calls, (unsigned long long)cell->elapsed_us,
                     (unsigned long long)cell->bytes, (unsigned long long)cell->pixels,
                     cell->errors, cell->pending, cell->flags_or,
                     (unsigned long long)cell->c2m_bytes, (unsigned long long)cell->m2c_bytes);
        }
    }
}

static void phase(const char *scene, unsigned kind, unsigned round,
                  size_t cache_bytes, bool strict_timer)
{
    const char *timer_mode = strict_timer ? "strict" : "legacy";
    uint32_t *refresh_us = heap_caps_malloc(AB_FRAMES * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(refresh_us);
    ab_cost_stats_t *costs = heap_caps_malloc(sizeof(*costs), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(costs);
    ESP_ERROR_CHECK(esp_lv_adapter_lock(10000));
    assert(!s_refresh_in_progress && !s_manual_owned);
    s_refresh_tracking = false;
    s_strict_timer = strict_timer;
    lv_timer_pause(lv_display_get_refr_timer(s_display));
    ab_cost_probe_set_active(false);
    ab_cost_probe_set_background(false);
    ab_cost_probe_set_stage(AB_COST_STAGE_SETUP);
    cache_budget(cache_bytes);
    build_scene(kind);
    ab_probe_begin();
    for (unsigned i = 0; i < AB_WARMUP; i++) {
        update_scene(kind, i);
        manual_refresh();
        enforce_timer_policy();
        if ((i & 15) == 15) {
            esp_lv_adapter_unlock();
            vTaskDelay(1);
            ESP_ERROR_CHECK(esp_lv_adapter_lock(10000));
        }
    }
    esp_lv_adapter_unlock();
    vTaskDelay(pdMS_TO_TICKS(80)); /* Identical bounded warm-up drain, untimed. */
    const ab_probe_stats_t warm = ab_probe_snapshot();
    ab_probe_end();
#if AB_HAS_DPI_PROBE
    assert(warm.scanout_irqs > 0);
#else
    (void)warm;
#endif
    ESP_ERROR_CHECK(esp_lv_adapter_lock(10000));
    assert(!s_refresh_in_progress);
    const size_t free_before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t largest_internal_before = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#if AB_HAS_NATIVE
    lv_draw_ortho_reset_stats();
#endif
    s_refresh_stats = (ab_refresh_stats_t) {0};
    s_refresh_tracking = true;
    ab_cost_probe_reset();
    ab_probe_begin();
    ab_cost_probe_set_active(true);
    ESP_ERROR_CHECK(heap_caps_monitor_local_minimum_free_size_start());
    uint64_t lock_us = 0, update_us = 0, refresh_sum = 0, timer_policy_us = 0;
    uint64_t unlock_us = 0, yield_us = 0;
    unsigned idle_yields = 0;
    const int64_t started = esp_timer_get_time();
    /* Count this single handoff explicitly; then each measured frame owns the
     * complete lock/update/refresh/unlock sequence, as in V7. */
    ab_cost_probe_set_stage(AB_COST_STAGE_UNLOCK);
    const int64_t handoff_start = esp_timer_get_time();
    esp_lv_adapter_unlock();
    const uint64_t handoff_us = esp_timer_get_time() - handoff_start;
    for (uint32_t i = 0; i < AB_FRAMES; i++) {
        ab_cost_probe_set_stage(AB_COST_STAGE_LOCK);
        int64_t mark = esp_timer_get_time();
        ESP_ERROR_CHECK(esp_lv_adapter_lock(10000));
        lock_us += esp_timer_get_time() - mark;
        ab_cost_probe_set_stage(AB_COST_STAGE_UPDATE);
        mark = esp_timer_get_time();
        update_scene(kind, AB_WARMUP + i);
        update_us += esp_timer_get_time() - mark;
        ab_probe_frame(i + 1);
        ab_cost_probe_set_stage(AB_COST_STAGE_REFRESH);
        mark = esp_timer_get_time();
        manual_refresh();
        refresh_us[i] = (uint32_t)(esp_timer_get_time() - mark);
        refresh_sum += refresh_us[i];
        mark = esp_timer_get_time();
        enforce_timer_policy();
        timer_policy_us += esp_timer_get_time() - mark;
        ab_cost_probe_set_stage(AB_COST_STAGE_UNLOCK);
        mark = esp_timer_get_time();
        esp_lv_adapter_unlock();
        unlock_us += esp_timer_get_time() - mark;
        if ((i & 15) == 15) {
            ab_cost_probe_set_stage(AB_COST_STAGE_YIELD);
            mark = esp_timer_get_time();
            vTaskDelay(1);
            yield_us += esp_timer_get_time() - mark;
            idle_yields++;
        }
    }
    const int64_t produced_us = esp_timer_get_time() - started;
    ab_cost_probe_set_stage(AB_COST_STAGE_DRAIN);
    const int64_t drain_started = esp_timer_get_time();
    const int64_t deadline = drain_started + 500000;
    ab_probe_stats_t probe;
    do {
        probe = ab_probe_snapshot();
        if (!AB_HAS_DPI_PROBE || probe.last_selected == AB_FRAMES) break;
        vTaskDelay(1);
    } while (esp_timer_get_time() < deadline);
    const int64_t drain_us = esp_timer_get_time() - drain_started;
    ab_probe_end();
    const int64_t close_lock_start = esp_timer_get_time();
    ESP_ERROR_CHECK(esp_lv_adapter_lock(10000));
    const int64_t close_lock_us = esp_timer_get_time() - close_lock_start;
    s_refresh_tracking = false;
    ab_cost_probe_set_active(false);
    assert(!s_refresh_in_progress && !s_manual_owned);
    const ab_refresh_stats_t events = s_refresh_stats;
    *costs = ab_cost_probe_snapshot();
    lv_timer_pause(lv_display_get_refr_timer(s_display));
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
    const lv_draw_ortho_stats_t stats = *lv_draw_ortho_get_stats();
    cache_hits = stats.images_cached; cache_kept = stats.image_cache_bytes;
    turner_failed = stats.turner_failed; cores = stats.cores;
    tile_internal = stats.tile_internal; tiles_in_turn = stats.tiles_in_turn;
    batches = stats.batches; waits = stats.waits; bytes_turned = stats.bytes_turned;
    if (cache_bytes) assert(cache_hits && cache_kept);
    if (kind == 1) assert(cache_hits == 0);
#endif
    const size_t free_after = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t internal_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t largest_internal_after = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    esp_lv_adapter_unlock();
    const bool observed = AB_HAS_DPI_PROBE && probe.scanout_irqs && !probe.invalid_indices &&
                          !probe.failures && probe.submissions == AB_FRAMES && probe.last_selected == AB_FRAMES;
    const int64_t cohort_us = observed ? probe.last_selected_us - started : 0;
    ESP_LOGI(TAG, "AB_RESULT protocol=8 board=%s variant=%s timer_mode=%s batch=%u scene=%s round=%u frames=%u "
             "produced_us=%lld submitted_fps=%.4f refresh_mean_us=%.2f refresh_median_us=%u refresh_p95_us=%u refresh_worst_us=%u "
             "scanout_metric=%s selected=%u unselected=%u scanout_cohort_us=%lld scanout_fps=%.4f "
             "draw_calls=%u scanout_irqs=%u invalid_indices=%u failures=%u "
             "cache_budget=%u cache_hits=%u cache_bytes=%u cores=%u turner_failed=%u "
             "native_stats=%u tile_internal=%u tiles_in_turn=%u batches=%u waits=%u bytes_turned=%llu "
             "free_before=%u internal_before=%u free_after=%u internal_after=%u minimum_free=%u minimum_internal=%u "
             "largest_internal_before=%u largest_internal_after=%u heap_ok=%u idle_yields=%u tick_hz=%u",
             AB_BOARD_NAME, AB_VARIANT, timer_mode, AB_BATCH_LIMIT, scene, round, AB_FRAMES,
             (long long)produced_us, AB_FRAMES * 1e6 / produced_us, refresh_sum / (double)AB_FRAMES,
             median, p95, worst, observed ? "GENERATION_SELECTED_FOR_DMA" : "UNAVAILABLE",
             probe.selected, probe.submissions >= probe.selected ? probe.submissions - probe.selected : 0,
             (long long)cohort_us, cohort_us > 0 ? probe.selected * 1e6 / cohort_us : -1.0,
             probe.draw_calls, probe.scanout_irqs, probe.invalid_indices, probe.failures,
             (unsigned)cache_bytes, cache_hits, cache_kept, cores, turner_failed,
             AB_HAS_NATIVE, tile_internal, tiles_in_turn, batches, waits, (unsigned long long)bytes_turned,
             (unsigned)free_before, (unsigned)internal_before, (unsigned)free_after, (unsigned)internal_after,
             (unsigned)minimum_free, (unsigned)minimum_internal, (unsigned)largest_internal_before,
             (unsigned)largest_internal_after, (unsigned)heap_ok, idle_yields, CONFIG_FREERTOS_HZ);
    const uint64_t accounted_us = handoff_us + lock_us + update_us + refresh_sum + timer_policy_us + unlock_us + yield_us;
    const int64_t residual_us = produced_us - (int64_t)accounted_us;
    ESP_LOGI(TAG, "AB_TIMING protocol=8 board=%s variant=%s timer_mode=%s scene=%s round=%u frames=%u "
             "handoff_us=%llu lock_us=%llu update_us=%llu refresh_us=%llu timer_policy_us=%llu "
             "unlock_us=%llu yield_us=%llu residual_us=%lld produced_us=%lld drain_us=%lld close_lock_us=%lld "
             "lock_calls=%u update_calls=%u refresh_calls=%u unlock_calls=%u yield_calls=%u",
             AB_BOARD_NAME, AB_VARIANT, timer_mode, scene, round, AB_FRAMES,
             (unsigned long long)handoff_us, (unsigned long long)lock_us, (unsigned long long)update_us,
             (unsigned long long)refresh_sum, (unsigned long long)timer_policy_us,
             (unsigned long long)unlock_us, (unsigned long long)yield_us,
             (long long)residual_us, (long long)produced_us, (long long)drain_us, (long long)close_lock_us,
             AB_FRAMES, AB_FRAMES, AB_FRAMES, AB_FRAMES, idle_yields);
    ESP_LOGI(TAG, "AB_REFRESH protocol=8 board=%s variant=%s timer_mode=%s scene=%s round=%u "
             "window=trace_plus_drain_and_close_lock manual_starts=%u manual_ready=%u automatic_starts=%u automatic_ready=%u "
             "manual_us=%llu automatic_us=%llu requests=%u requests_running=%u strict_pauses=%u "
             "nested=%u unmatched_ready=%u inclusive=1",
             AB_BOARD_NAME, AB_VARIANT, timer_mode, scene, round,
             events.manual_starts, events.manual_ready, events.automatic_starts, events.automatic_ready,
             (unsigned long long)events.manual_us, (unsigned long long)events.automatic_us,
             events.requests, events.requests_running, events.strict_pauses, events.nested, events.unmatched_ready);
    log_costs(scene, timer_mode, round, costs);
    assert(heap_ok && !turner_failed && !probe.failures && residual_us >= 0);
    assert(events.manual_starts == AB_FRAMES && events.manual_ready == AB_FRAMES);
    assert(events.automatic_starts == events.automatic_ready && !events.nested && !events.unmatched_ready);
    if (strict_timer) assert(events.automatic_starts == 0);
#if AB_HAS_DPI_PROBE
    assert(observed);
#endif
    heap_caps_free(refresh_us);
    heap_caps_free(costs);
    ESP_ERROR_CHECK(esp_lv_adapter_lock(10000));
    lv_obj_clean(lv_display_get_screen_active(s_display));
    lv_image_cache_drop(&s_image);
    cache_budget(0);
    lv_timer_pause(lv_display_get_refr_timer(s_display));
    esp_lv_adapter_unlock();
}

static void paired_phase(const char *scene, unsigned kind, unsigned round, size_t cache_bytes)
{
    /* Counterbalance order within each firmware. Each member rebuilds and warms
     * the same scene and uses exactly the same frame-index trace. */
    const bool strict_first = (round & 1u) == 0;
    phase(scene, kind, round, cache_bytes, strict_first);
    phase(scene, kind, round, cache_bytes, !strict_first);
}

void app_main(void)
{
    ESP_LOGI(TAG, "AB_READY protocol=8 board=%s variant=%s source_commit=%s source_tree=%s "
             "logical=%ux%u rotation=%u panel_buffers=3 cpu_mhz=%u psram_mhz=%u l2_kib=%u "
             "batch=%u rounds=%u warmup=%u frames=%u dpi_probe=%u dashboard_full=%u sensor_capture=0 "
             "draw_units=%u ppa_draw_requested=%u timer_modes=legacy,strict paired_order=alternating "
             "cost_probe=1 refresh_events=1 timings=lock,update,refresh,unlock,yield",
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
    ESP_ERROR_CHECK(esp_lv_adapter_lock(10000));
    /* Default and adapter handlers are already registered. This observer runs
     * after LVGL's default REF_REQUEST resume handler. */
    lv_display_add_event_cb(s_display, refresh_event, LV_EVENT_ALL, NULL);
    lv_timer_pause(lv_display_get_refr_timer(s_display));
    esp_lv_adapter_unlock();
    ESP_ERROR_CHECK(esp_lv_adapter_start());
    prepare_bank();
    for (unsigned round = 1; round <= AB_ROUNDS; round++) {
        paired_phase("dashboard", 0, round, 0);
        paired_phase("changing_rgb565", 1, round, 0);
        paired_phase("fixed_flash_off", 2, round, 0);
#if AB_HAS_NATIVE
        paired_phase("fixed_flash_warm_cache", 2, round, 4 * 1024 * 1024);
#endif
    }
    for (unsigned i = 0; i < 4; i++) heap_caps_free(s_bank[i]);
    ESP_LOGI(TAG, "AB_DONE protocol=8 board=%s variant=%s rounds=%u timer_modes=legacy,strict "
             "hardware_measurement=COMPLETE compare_only_matching_board_scene_and_timer_mode=1",
             AB_BOARD_NAME, AB_VARIANT, AB_ROUNDS);
    while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}
