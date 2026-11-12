/* SPDX-FileCopyrightText: 2026 youkorr
 * SPDX-License-Identifier: MIT
 * FreeRTOS can defer task-memory reclamation to an idle task.
 */
#pragma once

static UBaseType_t s_pr818_tasks_before_test;
static UBaseType_t s_pr818_tasks_after_settle;
static uint32_t s_pr818_settle_ms;

static bool pr818_wait_deleted_tasks(UBaseType_t expected)
{
    const TickType_t started = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(200);
    unsigned stable = 0;
    while ((TickType_t)(xTaskGetTickCount() - started) < timeout) {
        /* Check twice after yielding, so idle cleanup has time to finish
         * freeing a TCB after removing it from the termination list. */
        vTaskDelay(pdMS_TO_TICKS(10));
        s_pr818_tasks_after_settle = uxTaskGetNumberOfTasks();
        if (s_pr818_tasks_after_settle <= expected)
            stable++;
        else
            stable = 0;
        if (stable >= 2) {
            s_pr818_settle_ms = (uint32_t)(xTaskGetTickCount() - started) * portTICK_PERIOD_MS;
            return true;
        }
    }
    s_pr818_settle_ms = (uint32_t)(xTaskGetTickCount() - started) * portTICK_PERIOD_MS;
    return false;
}
