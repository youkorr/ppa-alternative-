#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 youkorr
# SPDX-License-Identifier: MIT
"""Compile the actual counter module against a deterministic host ISR model."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent
HEADERS = {
    'esp_attr.h': '#pragma once\n#define DRAM_ATTR\n#define IRAM_ATTR\n',
    'esp_timer.h': '#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n',
    'freertos/FreeRTOS.h': '#pragma once\n',
    'freertos/portmacro.h': '''#pragma once
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define portENTER_CRITICAL_ISR(x) ((void)(x))
#define portEXIT_CRITICAL_ISR(x) ((void)(x))
''',
    'esp_lcd_panel_ops.h': '''#pragma once
typedef void *esp_lcd_panel_handle_t;
typedef int esp_err_t;
#define ESP_OK 0
'''
}
MODEL = r'''
#include "ab_scanout_probe.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
static uint8_t pixels[3][128], external[128];
static void *fb[3] = {pixels[0],pixels[1],pixels[2]};
static void *panel = (void *)123;
static int error;
static int inject_old_index = -1;
int64_t esp_timer_get_time(void) { static int64_t now; return now += 100; }
esp_err_t __real_esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t p, int a,int b,int c,int d,const void *data)
{
 (void)p;(void)a;(void)b;(void)c;(void)d;(void)data;
 if(inject_old_index >= 0) ab_scanout_latched_from_isr(panel,(unsigned)inject_old_index);
 return error;
}
esp_err_t __wrap_esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t,int,int,int,int,const void *);
static void submit(unsigned gen,unsigned index)
{
 ab_probe_frame(gen);
 assert(__wrap_esp_lcd_panel_draw_bitmap(panel,0,0,8,8,pixels[index]) == ESP_OK);
}
int main(void)
{
 ab_probe_bind(panel,fb,128);ab_probe_begin();
 submit(1,0);
 for(int i=0;i<60;i++) ab_scanout_latched_from_isr(panel,0);
 ab_probe_stats_t s=ab_probe_snapshot();assert(s.selected==1 && s.scanout_irqs==60 && s.submissions==1);
 /* Old-index IRQ between generation tag and driver selection must not count
  * the next generation prematurely. This models the wrapper/ISR boundary. */
 inject_old_index=0;submit(2,1);inject_old_index=-1;
 assert(ab_probe_snapshot().selected==1);
 ab_scanout_latched_from_isr(panel,1);assert(ab_probe_snapshot().selected==2);
 /* Skip generation3 entirely: report actual selections rather than submissions. */
 submit(3,0);submit(4,2);ab_scanout_latched_from_isr(panel,2);
 s=ab_probe_snapshot();assert(s.submissions==4 && s.selected==3 && s.last_selected==4);
 /* Multiple draw regions of one application generation count once. */
 assert(__wrap_esp_lcd_panel_draw_bitmap(panel,0,0,8,8,pixels[2]+16)==ESP_OK);
 ab_scanout_latched_from_isr(panel,2);assert(ab_probe_snapshot().selected==3);
 assert(ab_probe_snapshot().submissions==4);
 /* Reusing an earlier framebuffer index still carries the new generation. */
 inject_old_index=2;submit(5,0);inject_old_index=-1;
 assert(ab_probe_snapshot().selected==3);
 ab_scanout_latched_from_isr(panel,0);assert(ab_probe_snapshot().selected==4);
 ab_scanout_latched_from_isr((void *)456,0);assert(ab_probe_snapshot().selected==4);
 ab_probe_frame(6);
 assert(__wrap_esp_lcd_panel_draw_bitmap(panel,0,0,8,8,external)==ESP_OK);
 assert(ab_probe_snapshot().invalid_indices==1);
 error=1;assert(__wrap_esp_lcd_panel_draw_bitmap(panel,0,0,8,8,pixels[1])==1);error=0;
 assert(ab_probe_snapshot().failures==1);
 ab_probe_end();ab_scanout_latched_from_isr(panel,1);assert(ab_probe_snapshot().failures==1);
 ab_probe_begin();ab_scanout_latched_from_isr(panel,0);
 s=ab_probe_snapshot();assert(s.selected==0 && s.submissions==0 && s.failures==0);
 submit(1,2);ab_scanout_latched_from_isr(panel,2);assert(ab_probe_snapshot().selected==1);
 puts("PASS: actual counter filters repeated IRQs, skipped and reused generations, boundary IRQs, invalid pointers and failed calls");
 return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='ab-probe-model-') as temporary:
    folder = Path(temporary)
    for name, text in HEADERS.items():
        p = folder / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
    (folder / 'model.c').write_text(MODEL)
    binary = folder / 'test'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I' + str(folder),
                    '-I' + str(ROOT), ROOT / 'ab_scanout_probe.c', folder / 'model.c', '-o', binary], check=True)
    subprocess.run([binary], check=True)
