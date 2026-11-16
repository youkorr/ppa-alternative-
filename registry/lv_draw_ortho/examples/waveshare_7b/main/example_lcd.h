/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 youkorr
 * Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
 */
#pragma once

#include "esp_err.h"
#include "esp_lcd_types.h"

#define EXAMPLE_LCD_H_RES 1024
#define EXAMPLE_LCD_V_RES 600

/* The panel, with `num_fbs` frame buffers of its own in PSRAM. Another board:
 * replace example_lcd.c, keep this. */
esp_err_t example_lcd_init(uint32_t num_fbs, esp_lcd_panel_handle_t *panel);
