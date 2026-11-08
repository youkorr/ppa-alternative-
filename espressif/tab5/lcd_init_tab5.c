/*
 * SPDX-License-Identifier: Apache-2.0
 * The panel setup below follows Espressif's M5Stack Tab5 BSP (esp-bsp,
 * bsp/m5stack_tab5/src/bsp_display.c, Apache-2.0); disp_init_data_ili9881c.h
 * is its ILI9881C init table, unchanged.
 * Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
 *
 * Replaces esp_lvgl_adapter's test_apps/lvgl9 components/hw_init/lcd/
 * lcd_init_mipi_dsi.c (the EV board's EK79007) for an M5Stack Tab5 with the
 * ILI9881C panel (the first Tab5; ESPHome's model M5Stack-Tab5), 720x1280,
 * two DSI lanes. Its reset and power line is pin 4 of the PI4IOE5V6408 at
 * 0x43 on I2C (SDA GPIO31, SCL GPIO32); the backlight is GPIO22.
 */

#include "hw_init.h"

#if CONFIG_EXAMPLE_LCD_INTERFACE_MIPI_DSI

#include "esp_check.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9881c.h"
#include "esp_io_expander_pi4ioe5v6408.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "disp_init_data_ili9881c.h"

static const char *TAG = "hw_lcd_init";

#define TAB5_LDO_MIPI_CHAN          (3)
#define TAB5_LDO_MIPI_VOLTAGE_MV    (2500)
#define TAB5_I2C_SDA                (31)
#define TAB5_I2C_SCL                (32)
#define TAB5_LCD_RESET_PIN          (IO_EXPANDER_PIN_NUM_4)
#define TAB5_BACKLIGHT_GPIO         (22)
#define TAB5_DSI_LANES              (2)
#define TAB5_DSI_LANE_MBPS          (1000)

static esp_lcd_dsi_bus_handle_t s_mipi_dsi_bus;
static esp_lcd_panel_io_handle_t s_mipi_dbi_io;
static esp_lcd_panel_handle_t s_panel_handle;
static esp_ldo_channel_handle_t s_ldo_mipi_phy;
/* Kept across the test app's display restarts: neither holds any LCD state. */
static i2c_master_bus_handle_t s_i2c;
static esp_io_expander_handle_t s_expander;
static bool s_backlight_ready;

/* Once for the whole run: the test app's warm-up display start in app_main
 * makes these before the first test counts its heap. */
static esp_err_t tab5_board_once(void)
{
    if (!s_i2c) {
        const i2c_master_bus_config_t i2c_config = {
            .i2c_port = -1,
            .sda_io_num = TAB5_I2C_SDA,
            .scl_io_num = TAB5_I2C_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .flags.enable_internal_pullup = true,
        };
        ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_config, &s_i2c), TAG, "I2C");
    }
    if (!s_expander) {
        ESP_RETURN_ON_ERROR(esp_io_expander_new_i2c_pi4ioe5v6408(s_i2c, ESP_IO_EXPANDER_I2C_PI4IOE5V6408_ADDRESS_LOW,
                                                                 &s_expander), TAG, "IO expander");
    }
    if (!s_backlight_ready) {
        const ledc_timer_config_t timer = {
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .duty_resolution = LEDC_TIMER_10_BIT,
            .timer_num = LEDC_TIMER_0,
            .freq_hz = 5000,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        const ledc_channel_config_t channel = {
            .gpio_num = TAB5_BACKLIGHT_GPIO,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = LEDC_CHANNEL_1,
            .intr_type = LEDC_INTR_DISABLE,
            .timer_sel = LEDC_TIMER_0,
            .duty = 1023,
            .hpoint = 0,
        };
        ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
        ESP_RETURN_ON_ERROR(ledc_channel_config(&channel), TAG, "backlight");
        s_backlight_ready = true;
    }
    return ESP_OK;
}

esp_err_t hw_lcd_init(esp_lcd_panel_handle_t *panel_handle, esp_lcd_panel_io_handle_t *io_handle,
                      esp_lv_adapter_tear_avoid_mode_t tear_avoid_mode, esp_lv_adapter_rotation_t rotation)
{
    ESP_ERROR_CHECK(tab5_board_once());

    /* Reset the panel through the IO expander, as ESPHome's M5Stack-Tab5 does.
     * The PI4IOE5V6408 starts with every output high-impedance and pulled
     * down, so a level written alone never reaches the pin and the panel stays
     * held in reset: the output has to be switched on (push-pull) as well. */
    ESP_ERROR_CHECK(esp_io_expander_set_dir(s_expander, TAB5_LCD_RESET_PIN, IO_EXPANDER_OUTPUT));
    ESP_ERROR_CHECK(esp_io_expander_set_level(s_expander, TAB5_LCD_RESET_PIN, 0));
    ESP_ERROR_CHECK(esp_io_expander_set_output_mode(s_expander, TAB5_LCD_RESET_PIN, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL));
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_ERROR_CHECK(esp_io_expander_set_level(s_expander, TAB5_LCD_RESET_PIN, 1));
    vTaskDelay(pdMS_TO_TICKS(120));

    if (!s_ldo_mipi_phy) {
        const esp_ldo_channel_config_t ldo_mipi_phy_config = {
            .chan_id = TAB5_LDO_MIPI_CHAN,
            .voltage_mv = TAB5_LDO_MIPI_VOLTAGE_MV,
        };
        ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_mipi_phy_config, &s_ldo_mipi_phy));
    }

    const esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = TAB5_DSI_LANES,
        .phy_clk_src = 0,
        .lane_bit_rate_mbps = TAB5_DSI_LANE_MBPS,
    };
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &s_mipi_dsi_bus));

    ESP_LOGI(TAG, "Install panel IO");
    const esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(s_mipi_dsi_bus, &dbi_config, &s_mipi_dbi_io));

    ESP_LOGI(TAG, "Install LCD driver of ili9881c (M5Stack Tab5)");
    esp_lcd_dpi_panel_config_t dpi_config = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 60,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = esp_lv_adapter_get_required_frame_buffer_count(tear_avoid_mode, rotation),
        .video_timing = {
            .h_size = HW_LCD_H_RES,
            .v_size = HW_LCD_V_RES,
            .hsync_back_porch = 140,
            .hsync_pulse_width = 40,
            .hsync_front_porch = 40,
            .vsync_back_porch = 20,
            .vsync_pulse_width = 4,
            .vsync_front_porch = 20,
        },
    };
    const ili9881c_vendor_config_t vendor_config = {
        .init_cmds = disp_init_data_ili9881c,
        .init_cmds_size = sizeof(disp_init_data_ili9881c) / sizeof(disp_init_data_ili9881c[0]),
        .mipi_config = {
            .dsi_bus = s_mipi_dsi_bus,
            .dpi_config = &dpi_config,
            .lane_num = TAB5_DSI_LANES,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor_config,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9881c(s_mipi_dbi_io, &panel_config, &s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel_handle));
    (void)esp_lcd_panel_disp_on_off(s_panel_handle, true);

    *panel_handle = s_panel_handle;
    if (io_handle) {
        *io_handle = s_mipi_dbi_io;
    }
    return ESP_OK;
}

esp_err_t hw_lcd_deinit(void)
{
    if (s_panel_handle) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_del(s_panel_handle), TAG, "Failed to delete panel");
        s_panel_handle = NULL;
    }
    if (s_mipi_dbi_io) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_del(s_mipi_dbi_io), TAG, "Failed to delete panel IO");
        s_mipi_dbi_io = NULL;
    }
    if (s_mipi_dsi_bus) {
        ESP_RETURN_ON_ERROR(esp_lcd_del_dsi_bus(s_mipi_dsi_bus), TAG, "Failed to delete DSI bus");
        s_mipi_dsi_bus = NULL;
    }
    if (s_ldo_mipi_phy) {
        ESP_RETURN_ON_ERROR(esp_ldo_release_channel(s_ldo_mipi_phy), TAG, "Failed to release MIPI PHY LDO");
        s_ldo_mipi_phy = NULL;
    }
    return ESP_OK;
}

#endif
