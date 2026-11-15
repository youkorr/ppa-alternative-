/* SPDX-FileCopyrightText: 2026 youkorr
 * SPDX-License-Identifier: MIT
 */
#include "ab_scanout_probe.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

typedef struct {
    uintptr_t start;
    uint32_t generation;
} ab_slot_t;

static DRAM_ATTR portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR esp_lcd_panel_handle_t s_panel;
static DRAM_ATTR ab_slot_t s_slots[3];
static DRAM_ATTR size_t s_bytes;
static DRAM_ATTR uint32_t s_generation;
static DRAM_ATTR bool s_active;
static DRAM_ATTR ab_probe_stats_t s_stats;

void ab_probe_bind(esp_lcd_panel_handle_t panel, void *const framebuffers[3], size_t bytes)
{
    portENTER_CRITICAL(&s_lock);
    s_panel = panel;
    s_bytes = bytes;
    for (unsigned i = 0; i < 3; i++) {
        s_slots[i].start = (uintptr_t)framebuffers[i];
        s_slots[i].generation = 0;
    }
    s_active = false;
    portEXIT_CRITICAL(&s_lock);
}

void ab_probe_begin(void)
{
    portENTER_CRITICAL(&s_lock);
    s_stats = (ab_probe_stats_t) {0};
    for (unsigned i = 0; i < 3; i++) s_slots[i].generation = 0;
    s_generation = 0;
    s_active = true;
    portEXIT_CRITICAL(&s_lock);
}

void ab_probe_frame(uint32_t generation)
{
    portENTER_CRITICAL(&s_lock);
    s_generation = generation;
    portEXIT_CRITICAL(&s_lock);
}

void ab_probe_end(void)
{
    portENTER_CRITICAL(&s_lock);
    s_active = false;
    portEXIT_CRITICAL(&s_lock);
}

ab_probe_stats_t ab_probe_snapshot(void)
{
    portENTER_CRITICAL(&s_lock);
    const ab_probe_stats_t result = s_stats;
    portEXIT_CRITICAL(&s_lock);
    return result;
}

esp_err_t __real_esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel, int x1, int y1,
                                         int x2, int y2, const void *pixels);

esp_err_t __wrap_esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel, int x1, int y1,
                                         int x2, int y2, const void *pixels)
{
    /* Tag before the real call selects its new cur_fb_index. With the tested
     * tear-free three-buffer paths the submitted buffer is not active. The
     * ISR may run between this tag and selection; it still sees the old index.
     * Multiple driver calls for the same application generation count once. */
    portENTER_CRITICAL(&s_lock);
    if (s_active && panel == s_panel && s_generation) {
        const uintptr_t address = (uintptr_t)pixels;
        unsigned index;
        for (index = 0; index < 3; index++) {
            if (address >= s_slots[index].start && address < s_slots[index].start + s_bytes) break;
        }
        s_stats.draw_calls++;
        if (index == 3) {
            /* A copying draw path cannot be associated with a unique selected
             * framebuffer by this probe. Do not report an apparent FPS. */
            s_stats.invalid_indices++;
        } else {
            s_slots[index].generation = s_generation;
            if (s_stats.last_submitted != s_generation) {
                s_stats.last_submitted = s_generation;
                s_stats.submissions++;
            }
        }
    }
    portEXIT_CRITICAL(&s_lock);
    const esp_err_t result = __real_esp_lcd_panel_draw_bitmap(panel, x1, y1, x2, y2, pixels);
    if (result != ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        if (s_active && panel == s_panel) s_stats.failures++;
        portEXIT_CRITICAL(&s_lock);
    }
    return result;
}

void IRAM_ATTR ab_scanout_latched_from_isr(esp_lcd_panel_handle_t panel, unsigned index)
{
    portENTER_CRITICAL_ISR(&s_lock);
    if (s_active && panel == s_panel) {
        s_stats.scanout_irqs++;
        if (index >= 3) {
            s_stats.invalid_indices++;
        } else {
            const uint32_t generation = s_slots[index].generation;
            if (generation > s_stats.last_selected) {
                s_stats.last_selected = generation;
                s_stats.selected++;
                s_stats.last_selected_us = esp_timer_get_time();
            }
        }
    }
    portEXIT_CRITICAL_ISR(&s_lock);
}
