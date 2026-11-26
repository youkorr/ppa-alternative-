/*
 * SPDX-License-Identifier: CC0-1.0
 *
 * The Waveshare ESP32-P4-WIFI6-Touch-LCD-7B's panel (EK79007, 1024x600, two
 * MIPI DSI lanes), from esp-iot-solution's esp_lvgl_adapter test app
 * (SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD,
 * CC0-1.0), with the frame buffer count asked for.
 */
#include "example_lcd.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_idf_version.h"
#include "esp_lcd_ek79007.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"

#define LDO_MIPI_CHAN 3
#define LDO_MIPI_MV 2500
#define PIN_LCD_RST 33
#define PIN_BACKLIGHT 32   /* on when low */

esp_err_t example_lcd_init(uint32_t num_fbs, esp_lcd_panel_handle_t *panel)
{
    static esp_ldo_channel_handle_t ldo;
    const esp_ldo_channel_config_t ldo_config = {.chan_id = LDO_MIPI_CHAN, .voltage_mv = LDO_MIPI_MV};
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_config, &ldo), "lcd", "MIPI PHY LDO");

    esp_lcd_dsi_bus_handle_t bus;
    const esp_lcd_dsi_bus_config_t bus_config = EK79007_PANEL_BUS_DSI_2CH_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &bus), "lcd", "DSI bus");

    esp_lcd_panel_io_handle_t io;
    const esp_lcd_dbi_io_config_t dbi_config = EK79007_PANEL_IO_DBI_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(bus, &dbi_config, &io), "lcd", "DBI IO");

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    esp_lcd_dpi_panel_config_t dpi_config = EK79007_1024_600_PANEL_60HZ_CONFIG_CF(LCD_COLOR_FMT_RGB565);
#else
    esp_lcd_dpi_panel_config_t dpi_config = EK79007_1024_600_PANEL_60HZ_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB565);
#endif
    dpi_config.num_fbs = num_fbs;
    ek79007_vendor_config_t vendor_config = {
        .mipi_config = {.dsi_bus = bus, .dpi_config = &dpi_config},
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ek79007(io, &panel_config, panel), "lcd", "EK79007");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(*panel), "lcd", "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(*panel), "lcd", "init");

    const gpio_config_t backlight = {.pin_bit_mask = 1ULL << PIN_BACKLIGHT, .mode = GPIO_MODE_OUTPUT};
    ESP_RETURN_ON_ERROR(gpio_config(&backlight), "lcd", "backlight");
    gpio_set_level(PIN_BACKLIGHT, 0);
    return ESP_OK;
}
