/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "hw_init.h"

#if !CONFIG_EXAMPLE_LCD_INTERFACE_MIPI_DSI && \
    !CONFIG_EXAMPLE_LCD_INTERFACE_SPI_WITH_PSRAM && \
    !CONFIG_EXAMPLE_LCD_INTERFACE_SPI_WITHOUT_PSRAM
#error "No LCD interface selected! Please select one in menuconfig."
#endif

/* M5Stack Tab5 run: no input on purpose (see prepare.py). */
