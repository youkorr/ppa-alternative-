/* SPDX-FileCopyrightText: 2026 youkorr
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_lcd_panel_ops.h"

/* Observes frame generations selected for DMA scanout. This is not an optical
 * measurement of panel glass, nor a count of VSYNC interrupts. */
typedef struct {
    uint32_t submissions, draw_calls, selected, failures, scanout_irqs;
    uint32_t last_submitted, last_selected, invalid_indices;
    int64_t last_selected_us;
} ab_probe_stats_t;

void ab_probe_bind(esp_lcd_panel_handle_t panel, void *const framebuffers[3], size_t bytes);
void ab_probe_begin(void);
void ab_probe_frame(uint32_t generation);
void ab_probe_end(void);
ab_probe_stats_t ab_probe_snapshot(void);
/* Called by the private benchmark-only ESP-IDF DPI source probe. */
void ab_scanout_latched_from_isr(esp_lcd_panel_handle_t panel, unsigned framebuffer_index);
