#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 youkorr
# SPDX-License-Identifier: MIT
"""Add V8.1 post-measurement cleanup tracing to an isolated V8 main source."""
import argparse
import hashlib
import json
from pathlib import Path

SOURCE_SHA256 = '05b46f6c62b895327b433cc43c280382cb9353447ee13d6e04e78d3e3b24d9be'
MARKER = 'ab-cleanup-v8.1: post-measurement cleanup trace'
HELPERS = r'''
/* ab-cleanup-v8.1: post-measurement cleanup trace
 * This observer never changes, retries or interrupts a cleanup operation.
 * Stage snapshots are protected on both CPUs; no allocation/logging occurs
 * while the observer mux is held. The task is created only after the final
 * official measurement has emitted its last AB_COST line.
 */
typedef struct {
    uint32_t stage, edge, round;
    int32_t bank;
    int64_t since_us;
    bool active;
} ab_cleanup_progress_t;
static DRAM_ATTR portMUX_TYPE s_cleanup_mux = portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR ab_cleanup_progress_t s_cleanup_progress;
static const char *s_cleanup_scene = "final";
static const char *s_cleanup_mode = "none";
static DRAM_ATTR char s_cleanup_wait_format[] =
    "AB_CLEANUP_WAIT protocol=8 diagnostic=8.1 stage_id=%u edge_id=%u round=%u bank=%d elapsed_ms=%u\n";
static DRAM_ATTR char s_cleanup_complete_format[] =
    "AB_CLEANUP_COMPLETE protocol=8 diagnostic=8.1 status=finished\n";

static void cleanup_console_write(const char *line)
{
    /* This pinned IDF output helper only writes console characters; it does
     * not enter the panic handler or acquire ESP_LOG's mutex. It mirrors the
     * configured UART and USB Serial/JTAG secondary console. */
    panic_print_str(line);
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG || CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG
    /* Flush a short final line even when it does not fill a USB packet. */
    usb_serial_jtag_ll_txfifo_flush();
#endif
}

static ab_cleanup_progress_t cleanup_snapshot(void)
{
    ab_cleanup_progress_t progress;
    portENTER_CRITICAL_SAFE(&s_cleanup_mux);
    progress = s_cleanup_progress;
    portEXIT_CRITICAL_SAFE(&s_cleanup_mux);
    return progress;
}

static void cleanup_context(const char *scene, const char *mode, unsigned round)
{
    s_cleanup_scene = scene;
    s_cleanup_mode = mode;
    portENTER_CRITICAL_SAFE(&s_cleanup_mux);
    s_cleanup_progress.round = round;
    portEXIT_CRITICAL_SAFE(&s_cleanup_mux);
}

static void cleanup_mark(unsigned stage, const char *name, bool begin, int bank)
{
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL_SAFE(&s_cleanup_mux);
    s_cleanup_progress.stage = stage;
    s_cleanup_progress.edge = begin ? 0 : 1;
    s_cleanup_progress.bank = bank;
    s_cleanup_progress.since_us = now;
    const unsigned round = s_cleanup_progress.round;
    portEXIT_CRITICAL_SAFE(&s_cleanup_mux);
    ESP_LOGI(TAG, "AB_CLEANUP protocol=8 diagnostic=8.1 board=%s variant=%s scene=%s timer_mode=%s round=%u "
             "stage_id=%u stage=%s edge=%s bank=%d",
             AB_BOARD_NAME, AB_VARIANT, s_cleanup_scene, s_cleanup_mode, round,
             stage, name, begin ? "begin" : "end", bank);
}

static void cleanup_observer(void *argument)
{
    (void)argument;
    int64_t reported_since = -1;
    int64_t last_report = 0;
    for (;;) {
        const ab_cleanup_progress_t progress = cleanup_snapshot();
        if (!progress.active) {
            vTaskDelete(NULL);
            return;
        }
        const int64_t now = esp_timer_get_time();
        if (now - progress.since_us >= 5000000 &&
            (reported_since != progress.since_us || now - last_report >= 5000000)) {
            /* Raw console printing does not take ESP_LOG's lock. This is still a
             * best-effort task: a CPU-wide stall can prevent any observer.
             * Numeric stage/edge IDs avoid dereferencing stage-name strings.
             */
            char line[192];
            snprintf(line, sizeof(line), s_cleanup_wait_format,
                     progress.stage, progress.edge, progress.round, progress.bank,
                     (uint32_t)((now - progress.since_us) / 1000));
            cleanup_console_write(line);
            reported_since = progress.since_us;
            last_report = now;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void cleanup_observer_arm(void)
{
    cleanup_mark(1, "observer_create", true, -1);
    portENTER_CRITICAL_SAFE(&s_cleanup_mux);
    s_cleanup_progress.active = true;
    portEXIT_CRITICAL_SAFE(&s_cleanup_mux);
    const BaseType_t created = xTaskCreatePinnedToCore(cleanup_observer,
        "ab_cleanup", 4096, NULL, 6, NULL, tskNO_AFFINITY);
    if (created != pdPASS) {
        portENTER_CRITICAL_SAFE(&s_cleanup_mux);
        s_cleanup_progress.active = false;
        portEXIT_CRITICAL_SAFE(&s_cleanup_mux);
        ESP_LOGW(TAG, "AB_CLEANUP_WATCH protocol=8 diagnostic=8.1 status=unavailable create_result=%d", (int)created);
    } else {
        ESP_LOGI(TAG, "AB_CLEANUP_WATCH protocol=8 diagnostic=8.1 status=armed threshold_ms=5000 period_ms=1000");
    }
    cleanup_mark(1, "observer_create", false, -1);
}

static void cleanup_observer_disarm(void)
{
    portENTER_CRITICAL_SAFE(&s_cleanup_mux);
    s_cleanup_progress.active = false;
    portEXIT_CRITICAL_SAFE(&s_cleanup_mux);
}

'''


def once(text, before, after):
    if text.count(before) != 1:
        raise ValueError(f'Expected one pinned source anchor: {before[:80]!r}')
    return text.replace(before, after, 1)


def instrument(original):
    if MARKER.encode() in original:
        raise ValueError('Already instrumented; use a clean private V8 source')
    digest = hashlib.sha256(original).hexdigest()
    if digest != SOURCE_SHA256:
        raise ValueError(f'Unexpected source SHA256 {digest}; refusing an unaudited patch')
    text = original.decode()
    text = once(text, '#include "esp_timer.h"', '''#include "esp_timer.h"
#include "esp_attr.h"
#include "esp_private/panic_internal.h"
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG || CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG
#include "hal/usb_serial_jtag_ll.h"
#endif
#if CONFIG_ESP_SYSTEM_PANIC_SILENT_REBOOT
#error "Cleanup diagnostic requires the pinned visible-console output helper"
#endif''')
    text = once(text, 'static const char *TAG = "ab_bench";\n', 'static const char *TAG = "ab_bench";\n' + HELPERS)
    text = once(text, '    log_costs(scene, timer_mode, round, costs);\n', '''    log_costs(scene, timer_mode, round, costs);
    cleanup_context(scene, timer_mode, round);
    /* Only the final official phase can arm the observer. Earlier markers
     * remain outside measured intervals and allocate no additional task.
     */
    const bool final_cleanup = !AB_HAS_NATIVE && round == AB_ROUNDS && kind == 2 &&
                               strict_timer == ((round & 1u) != 0);
    if (final_cleanup) cleanup_observer_arm();
    cleanup_mark(2, "phase_assertions", true, -1);
''')
    text = once(text, '''    heap_caps_free(refresh_us);
    heap_caps_free(costs);
    ESP_ERROR_CHECK(esp_lv_adapter_lock(10000));
    lv_obj_clean(lv_display_get_screen_active(s_display));
    lv_image_cache_drop(&s_image);
    cache_budget(0);
    lv_timer_pause(lv_display_get_refr_timer(s_display));
    esp_lv_adapter_unlock();
}''', '''    cleanup_mark(2, "phase_assertions", false, -1);
    cleanup_mark(3, "free_refresh_samples", true, -1);
    heap_caps_free(refresh_us);
    cleanup_mark(3, "free_refresh_samples", false, -1);
    cleanup_mark(4, "free_cost_snapshot", true, -1);
    heap_caps_free(costs);
    cleanup_mark(4, "free_cost_snapshot", false, -1);
    cleanup_mark(5, "acquire_lvgl_lock", true, -1);
    ESP_ERROR_CHECK(esp_lv_adapter_lock(10000));
    cleanup_mark(5, "acquire_lvgl_lock", false, -1);
    cleanup_mark(6, "lv_obj_clean", true, -1);
    lv_obj_clean(lv_display_get_screen_active(s_display));
    cleanup_mark(6, "lv_obj_clean", false, -1);
    cleanup_mark(7, "lv_image_cache_drop", true, -1);
    lv_image_cache_drop(&s_image);
    cleanup_mark(7, "lv_image_cache_drop", false, -1);
    cleanup_mark(8, "reset_native_image_cache", true, -1);
    cache_budget(0);
    cleanup_mark(8, "reset_native_image_cache", false, -1);
    cleanup_mark(9, "pause_display_timer", true, -1);
    lv_timer_pause(lv_display_get_refr_timer(s_display));
    cleanup_mark(9, "pause_display_timer", false, -1);
    cleanup_mark(10, "release_lvgl_lock", true, -1);
    esp_lv_adapter_unlock();
    cleanup_mark(10, "release_lvgl_lock", false, -1);
    cleanup_mark(11, "phase_cleanup_complete", false, -1);
}''')
    text = once(text, '    assert(ab_wallpaper_end - ab_wallpaper_start == AB_WIDTH * AB_HEIGHT * 2);', '''    ESP_LOGI(TAG, "AB_CLEANUP_INFO protocol=8 diagnostic=8.1 stage_trace=1 observer_after_final_measurement=1 watch_threshold_ms=5000");
    assert(ab_wallpaper_end - ab_wallpaper_start == AB_WIDTH * AB_HEIGHT * 2);''')
    text = once(text, '    for (unsigned i = 0; i < 4; i++) heap_caps_free(s_bank[i]);\n', '''    cleanup_context("final", "none", AB_ROUNDS);
    for (unsigned i = 0; i < 4; i++) {
        cleanup_mark(20, "free_source_bank", true, (int)i);
        heap_caps_free(s_bank[i]);
        cleanup_mark(20, "free_source_bank", false, (int)i);
    }
    cleanup_mark(21, "emit_ab_done", true, -1);
''')
    text = once(text, '    while (true) vTaskDelay(pdMS_TO_TICKS(1000));\n}', '''    cleanup_mark(21, "emit_ab_done", false, -1);
    cleanup_console_write(s_cleanup_complete_format);
    cleanup_observer_disarm();
    while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}''')
    return text.encode()


def patch(source, output=None):
    original = source.read_bytes()
    result = instrument(original)
    target = output or source
    target.parent.mkdir(parents=True, exist_ok=True)
    # Break a possible cache hardlink before changing a private staging file.
    target.unlink(missing_ok=True)
    target.write_bytes(result)
    return {'diagnostic': '8.1', 'test_only': True, 'source_sha256': hashlib.sha256(original).hexdigest(),
            'instrumented_sha256': hashlib.sha256(result).hexdigest(),
            'measured_work_changed': False, 'cleanup_operations_reordered': False,
            'observer': 'best-effort task created after final official AB_COST; no abort or recovery action',
            'observer_console': 'IDF panic_print_str output helper only; UART plus USB secondary, explicit USB packet flush; no panic handler',
            'watch_threshold_ms': 5000, 'watch_interval_ms': 1000}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    print(json.dumps(patch(args.source, args.out), indent=2))
