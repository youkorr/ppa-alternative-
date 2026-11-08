#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 youkorr
# Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
#
# Makes esp_lvgl_adapter's test_apps/lvgl9 run on an M5Stack Tab5 (the first
# one: ILI9881C panel, 720x1280, ESPHome's model M5Stack-Tab5), and run by
# itself:
#
#   - lcd_init_tab5.c replaces the EV board's EK79007 setup, after
#     Espressif's own Tab5 BSP: I2C and a PI4IOE5V6408 for the panel's reset,
#     the ILI9881C init table, the backlight on GPIO22;
#   - no touch: nothing here needs a finger;
#   - the tests start at boot, after a timed full redraw (the Waveshare
#     harness's bench.inc.c);
#   - mode "copy": the test app without ROTATE_IN_DRAW;
#   - the angle under test, 270 (the Tab5 held in landscape) or 90.
#
#   prepare.py <test app dir> <rotate_in_draw|copy> [270|90]

import re
import shutil
import sys
from pathlib import Path

app, mode = Path(sys.argv[1]), sys.argv[2]
angle = sys.argv[3] if len(sys.argv) > 3 else "270"
assert mode in ("rotate_in_draw", "copy"), mode
assert angle in ("90", "180", "270"), angle
here = Path(__file__).resolve().parent


def sub(path, old, new):
    s = path.read_text()
    assert s.count(old) == 1, f"{path.name}: {old!r} found {s.count(old)} times"
    path.write_text(s.replace(old, new))


hw = app / "components/hw_init"
shutil.copyfile(here / "lcd_init_tab5.c", hw / "lcd/lcd_init_mipi_dsi.c")
shutil.copyfile(here / "disp_init_data_ili9881c.h", hw / "lcd/disp_init_data_ili9881c.h")

sub(hw / "idf_component.yml", "  espressif/esp_lcd_touch_gt911:\n",
    "  # M5Stack Tab5 run (prepare.py): its panel and the IO expander resetting it.\n"
    "  espressif/esp_lcd_ili9881c:\n"
    "    version: \"^1\"\n"
    "  espressif/esp_io_expander_pi4ioe5v6408:\n"
    "    version: \"^1\"\n"
    "\n"
    "  espressif/esp_lcd_touch_gt911:\n")
sub(hw / "CMakeLists.txt", '    list(APPEND srcs "lcd/lcd_init_mipi_dsi.c")\n',
    '    list(APPEND srcs "lcd/lcd_init_mipi_dsi.c")\n'
    '    # M5Stack Tab5 run (prepare.py)\n'
    '    list(APPEND component_requires espressif__esp_lcd_ili9881c espressif__esp_io_expander_pi4ioe5v6408\n'
    '         esp_driver_i2c esp_driver_ledc)\n')

# hw_init.c refuses a board with no input at all; this run has none on purpose.
sub(hw / "hw_init.c",
    '#if !HW_USE_TOUCH && !HW_USE_ENCODER\n#error "No input device configured!"\n#endif\n',
    "/* M5Stack Tab5 run: no input on purpose (see prepare.py). */\n")
sub(hw / "hw_init.h",
    "#define HW_LCD_H_RES    1024\n#define HW_LCD_V_RES    600\n#define HW_USE_TOUCH    1\n",
    "#define HW_LCD_H_RES    720    /* M5Stack Tab5 */\n"
    "#define HW_LCD_V_RES    1280\n"
    "#define HW_USE_TOUCH    0\n")

test = app / "main/test_esp_lvgl_adapter_features.c"
t = test.read_text()
func = re.compile(r"static esp_lv_adapter_rotation_t get_configured_rotation\(void\)\n\{.*?\n\}\n", re.S)
assert len(func.findall(t)) == 1, "get_configured_rotation() not found once"
t = func.sub("/* M5Stack Tab5 run: the angle under test (prepare.py). */\n"
             "static esp_lv_adapter_rotation_t get_configured_rotation(void)\n"
             "{\n"
             f"    return ESP_LV_ADAPTER_ROTATE_{angle};\n"
             "}\n", t)
test.write_text(t)
bench = (here.parent / "waveshare-7b/bench.inc.c").read_text()
bench = bench[bench.index("/* ----"):]   # without this repository's own header
old_title = '"esp_lvgl_adapter -- Waveshare 7B"'
assert bench.count(old_title) == 1
bench = bench.replace(old_title, '"esp_lvgl_adapter -- M5Stack Tab5"')
sub(test, "void app_main(void)\n", bench + "void app_main(void)\n")
sub(test, "    unity_run_menu();\n}",
    "    /* M5Stack Tab5 run: the timed redraw first, then every test, then the menu. */\n"
    "    unity_run_test_by_name(\"rotate bench: whole screen redrawn\");\n"
    "    unity_run_all_tests();\n"
    "    ESP_LOGI(TAG, \"M5Stack Tab5 run: all tests done\");\n"
    "    unity_run_menu();\n}")
print(f"prepared {app} for the M5Stack Tab5, mode {mode}, rotation {angle}")
