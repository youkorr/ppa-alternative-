/* SPDX-FileCopyrightText: 2026 youkorr
 * SPDX-License-Identifier: MIT
 * Short restart test; the wrappers observe the actual LVGL OS mutex.
 */
#include "src/core/lv_global.h"
#include "src/osal/lv_os_private.h"

#if LV_USE_OS != LV_OS_FREERTOS
#error "The hardware memory probe requires LVGL's FreeRTOS backend"
#endif

#define MEMORY_CYCLES 64
#define MEMORY_WARMUPS 4
#define MEMORY_BUDGET 256

static uint32_t s_mutex_created, s_mutex_deleted;
static size_t s_mutex_payload;
static int32_t s_mutex_returned;

lv_result_t __real_lv_mutex_init(lv_mutex_t *mutex);
lv_result_t __real_lv_mutex_delete(lv_mutex_t *mutex);

lv_result_t __wrap_lv_mutex_init(lv_mutex_t *mutex)
{
    const lv_result_t result = __real_lv_mutex_init(mutex);
    if (mutex == &LV_GLOBAL_DEFAULT()->lv_general_mutex && result == LV_RESULT_OK) {
        s_mutex_created++;
        if (mutex->xIsInitialized && mutex->xMutex)
            s_mutex_payload = heap_caps_get_allocated_size(mutex->xMutex);
    }
    return result;
}

lv_result_t __wrap_lv_mutex_delete(lv_mutex_t *mutex)
{
    const bool global = mutex == &LV_GLOBAL_DEFAULT()->lv_general_mutex;
    const size_t before = global ? heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) : 0;
    const lv_result_t result = __real_lv_mutex_delete(mutex);
    if (global && result == LV_RESULT_OK) {
        s_mutex_deleted++;
        s_mutex_returned = (int32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) - (int32_t)before;
    }
    return result;
}

void app_main(void)
{
    ESP_LOGI(TAG, "PR818_V5_READY board=%s: short memory test starts in 3 seconds", BOARD_NAME);
    vTaskDelay(pdMS_TO_TICKS(3000));
    /* Board-specific lazy allocations are outside the cumulative measurement. */
    test_init_display();
    test_deinit_display();
    const unsigned failures = Unity.TestFailures;
    for (unsigned i = 0; i < MEMORY_WARMUPS; i++) {
        unity_run_test_by_name("adapter render threads can be stopped repeatedly");
        if (Unity.TestFailures != failures) {
            ESP_LOGE(TAG, "PR818_ABORT reason=warmup cycle=%u", i + 1);
            unity_run_menu();
            return;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    const size_t baseline = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t internal_baseline = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t created_before = s_mutex_created, deleted_before = s_mutex_deleted;
    int32_t max_lost = 0, max_internal_lost = 0, lost = 0, internal_lost = 0;
    bool mutex_ok = true, heap_ok = true;
    ESP_LOGI(TAG, "PR818_MEMORY_START board=%s revision=%s cycles=%u warmups=%u budget=%u baseline=%u internal=%u",
             BOARD_NAME, BOARD_REVISION, MEMORY_CYCLES, MEMORY_WARMUPS, MEMORY_BUDGET,
             (unsigned)baseline, (unsigned)internal_baseline);
    for (unsigned cycle = 1; cycle <= MEMORY_CYCLES; cycle++) {
        const uint32_t created = s_mutex_created, deleted = s_mutex_deleted;
        unity_run_test_by_name("adapter render threads can be stopped repeatedly");
        if (Unity.TestFailures != failures) {
            ESP_LOGE(TAG, "PR818_ABORT reason=unity cycle=%u", cycle);
            unity_run_menu();
            return;
        }
        /* Let both idle tasks reclaim deferred FreeRTOS allocations. */
        vTaskDelay(pdMS_TO_TICKS(20));
        const size_t free_now = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        const size_t internal_now = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        lost = (int32_t)baseline - (int32_t)free_now;
        internal_lost = (int32_t)internal_baseline - (int32_t)internal_now;
        if (lost > max_lost) max_lost = lost;
        if (internal_lost > max_internal_lost) max_internal_lost = internal_lost;
        const uint32_t made = s_mutex_created - created, freed = s_mutex_deleted - deleted;
        const bool no_global_mutex = LV_GLOBAL_DEFAULT()->lv_general_mutex.xIsInitialized == pdFALSE;
        if (made != 1 || freed != 1 || !no_global_mutex) mutex_ok = false;
        if (cycle % 8 == 0 && !heap_caps_check_integrity_all(true)) heap_ok = false;
        ESP_LOGI(TAG, "PR818_MEMORY cycle=%u lost=%ld internal_lost=%ld mutex_init=%u mutex_delete=%u payload=%u returned=%ld",
                 cycle, (long)lost, (long)internal_lost, (unsigned)made, (unsigned)freed,
                 (unsigned)s_mutex_payload, (long)s_mutex_returned);
    }
    const uint32_t created = s_mutex_created - created_before, deleted = s_mutex_deleted - deleted_before;
    const bool passed = mutex_ok && heap_ok && max_lost <= MEMORY_BUDGET && max_internal_lost <= MEMORY_BUDGET &&
                        created == MEMORY_CYCLES && deleted == MEMORY_CYCLES;
    ESP_LOGI(TAG, "PR818_MEMORY_SUMMARY final_lost=%ld max_lost=%ld internal_final=%ld internal_max=%ld budget=%u heap_ok=%u",
             (long)lost, (long)max_lost, (long)internal_lost, (long)max_internal_lost, MEMORY_BUDGET, (unsigned)heap_ok);
    ESP_LOGI(TAG, "PR818_MUTEX_SUMMARY created=%u deleted=%u balanced=%u", (unsigned)created, (unsigned)deleted,
             (unsigned)mutex_ok);
    ESP_LOGI(TAG, "PR818_MEMORY_DONE result=%s cycles=%u", passed ? "PASS" : "FAIL", MEMORY_CYCLES);
    if (passed) {
        ESP_LOGI(TAG, "PR818_RESTART_OK count=64");
        ESP_LOGI(TAG, "PR818_DONE: memory test complete; save the full log");
    } else {
        ESP_LOGE(TAG, "PR818_ABORT reason=memory_or_mutex_budget");
    }
    unity_run_menu();
}
