#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 youkorr
# Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
#
# Makes esp_lvgl_adapter's test_apps/lvgl9 run on a Waveshare
# ESP32-P4-WIFI6-Touch-LCD-7B, and run by itself:
#
#   - the panel's reset is GPIO33 (GPIO27 on Espressif's EV board, the same
#     EK79007 1024x600 panel), and its backlight GPIO32, active low;
#   - no touch: the GT911 there has its reset on GPIO23, which the test app
#     does not drive, and nothing here needs a finger;
#   - the tests start at boot (the app waits for a key on the serial port
#     otherwise), after a timed full redraw (bench.inc.c);
#   - mode "copy": the test app without ROTATE_IN_DRAW at rotation 90, so
#     the same firmware shows what a rotated display costs today.
#
#   prepare.py <test app dir> <rotate_in_draw|copy>

import sys
from pathlib import Path

app, mode = Path(sys.argv[1]), sys.argv[2]
assert mode in ("rotate_in_draw", "copy"), mode
here = Path(__file__).resolve().parent


def sub(path, old, new):
    s = path.read_text()
    assert s.count(old) == 1, f"{path.name}: {old!r} found {s.count(old)} times"
    path.write_text(s.replace(old, new))


lcd = app / "components/hw_init/lcd/lcd_init_mipi_dsi.c"
sub(lcd, "#define HW_PIN_NUM_LCD_RST                      (27)",
    "#define HW_PIN_NUM_LCD_RST                      (33)   /* Waveshare 7B */")
sub(lcd, "    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel_handle));\n",
    "    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel_handle));\n"
    "\n"
    "    /* Waveshare 7B: the backlight is GPIO32, on when low. */\n"
    "    const gpio_config_t backlight = {\n"
    "        .pin_bit_mask = 1ULL << 32,\n"
    "        .mode = GPIO_MODE_OUTPUT,\n"
    "    };\n"
    "    ESP_ERROR_CHECK(gpio_config(&backlight));\n"
    "    gpio_set_level(32, 0);\n")

# hw_init.c refuses a board with no input at all; this run has none on purpose.
sub(app / "components/hw_init/hw_init.c",
    '#if !HW_USE_TOUCH && !HW_USE_ENCODER\n#error "No input device configured!"\n#endif\n',
    "/* Waveshare 7B run: no input on purpose (see prepare.py). */\n")
sub(app / "components/hw_init/hw_init.h",
    "#define HW_LCD_V_RES    600\n#define HW_USE_TOUCH    1\n",
    "#define HW_LCD_V_RES    600\n#define HW_USE_TOUCH    0   /* Waveshare 7B: GT911 reset not driven */\n")

test = app / "main/test_esp_lvgl_adapter_features.c"
if mode == "copy":
    sub(test, "static esp_lv_adapter_rotation_t get_configured_rotation(void)\n",
        "/* Waveshare 7B run, mode copy: rotation 90 without ROTATE_IN_DRAW. */\n"
        "#define CONFIG_EXAMPLE_DISPLAY_ROTATION_90 1\n\n"
        "static esp_lv_adapter_rotation_t get_configured_rotation(void)\n")
bench = (here / "bench.inc.c").read_text()
bench = bench[bench.index("/* ----"):]   # without this repository's own header
sub(test, "void app_main(void)\n", bench + "void app_main(void)\n")
sub(test, "    unity_run_menu();\n}",
    "    /* Waveshare 7B run: the timed redraw first, then every test, then the menu. */\n"
    "    unity_run_test_by_name(\"rotate bench: whole screen redrawn\");\n"
    "    unity_run_all_tests();\n"
    "    ESP_LOGI(TAG, \"Waveshare 7B run: all tests done\");\n"
    "    unity_run_menu();\n}")
print(f"prepared {app} for the Waveshare 7B, mode {mode}")
