/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 youkorr
 * Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
 *
 * LVGL configuration for the host tests: everything else is LVGL's default. */
#ifndef LV_CONF_H
#define LV_CONF_H

#ifndef LV_COLOR_DEPTH
#define LV_COLOR_DEPTH 16
#endif
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB
#define LV_USE_OS LV_OS_NONE
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1
#define LV_USE_FLOAT 1
#define LV_USE_MATRIX 1
#define LV_DRAW_TRANSFORM_USE_MATRIX 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_DRAW_SW_COMPLEX 1
#define LV_DRAW_LAYER_SIMPLE_BUF_SIZE (64 * 1024)
#define LV_DEF_REFR_PERIOD 33
#define LV_USE_PERF_MONITOR 0
#define LV_USE_SYSMON 0
#endif
