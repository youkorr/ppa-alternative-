<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 youkorr
Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-

A comment for esp-iot-solution PR #818, on what could not be tested here.
Everything below this block is what goes into the PR.
-->

Two configurations I could not test, for lack of the hardware:

- **ESP32-P4 rev 3.0.** Measured only below it: a Waveshare 7B (rev 1.3) and
  an M5Stack Tab5 (rev 1.0). This PR adds `test_adapter_esp32p4_rotate_in_draw`
  to `pytest_adapter_lvgl9.py`, on the
  `esp32p4_function_ev_board,screen,touch,eco_default` runner, so your CI
  runs the feature tests rotated 90 degrees on a rev 3 board. On rev 3.0 the
  PPA's 90/270 turn works in 32x32 blocks rather than 16x16, so I expect it to
  be faster there, but I have no figure.
- **RGB panels.** The option applies to RGB `DOUBLE_DIRECT` displays too
  (`esp_lcd_rgb_panel` takes up to three frame buffers and switches on its
  own refresh, like the DSI one), and the third-buffer path uses the same
  wait for the switch that `DOUBLE_DIRECT` already uses there, but it has
  never run on an RGB board. If you would rather not ship that untested, I
  can limit the option to MIPI DSI until it has been.

The timing harness I used (a 10 s whole-screen redraw timed from
`LV_EVENT_RENDER_START`) is in
https://github.com/youkorr/ppa-alternative-/tree/main/espressif (one folder
per board), if you want rev 3.0 figures from a board you have.
