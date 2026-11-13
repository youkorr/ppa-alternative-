#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 youkorr
# SPDX-License-Identifier: MIT
"""Install paired V9 eager/deferred engine-policy diagnostics into a board-prepared LVGL app.

This does not prepare/change a panel driver or fetch/build any dependencies.
Use patch_dpi_probe.py separately for the optional, same-on-all-variants private
IDF 5.5.6 scanout observer. All other files belong only to APP/main.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct

HERE = Path(__file__).resolve().parent


def install(args):
    native = args.variant == 'native'
    if args.stock_ppa and args.variant != 'official':
        raise ValueError('--stock-ppa is only valid with --variant official; native owns its PPA path')
    if not re.fullmatch(r'[0-9a-f]{40}', args.source_tree):
        raise ValueError('--source-tree must identify the exact candidate tree, including local integration')
    if not re.fullmatch(r'[0-9a-f]{40}', args.source_sha):
        raise ValueError('--source-sha must identify the source base/commit')
    width, height, rotation = (1280, 720, 270) if args.board == 'tab5' else (1024, 600, 180)
    variant = f'native{args.batch_limit}' if native else ('official_ppa1' if args.stock_ppa else args.variant)
    main = args.app / 'main'
    main.mkdir(parents=True, exist_ok=True)
    for name in ('ab_main.c', 'ab_scanout_probe.c', 'ab_scanout_probe.h', 'ab_cost_probe.c', 'ab_cost_probe.h'):
        shutil.copy2(HERE / name, main / name)
    header = {
        'AB_PROTOCOL_VERSION': 9, 'AB_BOARD_NAME': args.board, 'AB_VARIANT': variant,
        'AB_SOURCE_COMMIT': args.source_sha, 'AB_SOURCE_TREE': args.source_tree,
        'AB_WIDTH': width, 'AB_HEIGHT': height,
        'AB_BATCH_LIMIT': args.batch_limit if native else 0,
        'AB_HAS_NATIVE': int(native), 'AB_HAS_DPI_PROBE': int(args.probe),
        'AB_STOCK_PPA': int(args.stock_ppa),
        'AB_DASHBOARD_FULL': int(args.dashboard_full),
        'AB_FRAMES': args.frames, 'AB_ROUNDS': args.rounds, 'AB_WARMUP': args.warmup,
    }
    config = ''.join('#define ' + key + ' ' + (json.dumps(value) if isinstance(value, str) else str(value) + 'u') + '\n'
                     for key, value in header.items())
    config += f'#define AB_ROTATION ESP_LV_ADAPTER_ROTATE_{rotation}\n'
    config += '#define AB_TEAR_MODE ESP_LV_ADAPTER_TEAR_AVOID_MODE_' + ('DOUBLE_DIRECT' if native else 'TRIPLE_PARTIAL') + '\n'
    (main / 'ab_bench_config.h').write_text(config)
    pixels = bytearray(width * height * 2)
    for y in range(height):
        for x in range(width):
            r, g, b = x * 31 // width, y * 63 // height, ((x // 48) ^ (y // 48)) & 31
            struct.pack_into('<H', pixels, (y * width + x) * 2, r << 11 | g << 5 | b)
    (main / 'ab_wallpaper.rgb565').write_bytes(pixels)
    cmake = '''idf_component_register(
    SRCS "ab_main.c" "ab_scanout_probe.c" "ab_cost_probe.c"
    INCLUDE_DIRS "."
    REQUIRES hw_init esp_timer esp_mm esp_driver_ppa
    EMBED_FILES "ab_wallpaper.rgb565"
)
# Observe the real panel submissions without changing the adapter source.
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=esp_lcd_panel_draw_bitmap"
    "-Wl,--wrap=lv_draw_buf_copy"
    "-Wl,--wrap=esp_cache_msync"
    "-Wl,--wrap=ppa_do_scale_rotate_mirror"
    "-Wl,--wrap=ppa_do_fill"
    "-Wl,--wrap=ppa_do_blend"
)
'''
    if native:
        cmake += '''
idf_build_get_property(_ab_components BUILD_COMPONENTS)
if("esp_lvgl_adapter" IN_LIST _ab_components)
    set(_ab_adapter esp_lvgl_adapter)
elseif("espressif__esp_lvgl_adapter" IN_LIST _ab_components)
    set(_ab_adapter espressif__esp_lvgl_adapter)
else()
    message(FATAL_ERROR "A/B benchmark cannot identify the actual adapter library")
endif()
idf_component_get_property(_ab_adapter_lib ${_ab_adapter} COMPONENT_LIB)
'''
        cmake += f'target_compile_definitions(${{_ab_adapter_lib}} PRIVATE LV_DRAW_ORTHO_MAX_BATCH={args.batch_limit})\n'
    (main / 'CMakeLists.txt').write_text(cmake)
    sdkconfig = '''# A/B benchmark profile: keep these performance settings identical.
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_360=y
CONFIG_CACHE_L2_CACHE_256KB=n
CONFIG_CACHE_L2_CACHE_128KB=y
CONFIG_SPIRAM_SPEED_200M=y
CONFIG_SPIRAM_XIP_FROM_PSRAM=n
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=n
CONFIG_SPIRAM_RODATA=n
CONFIG_LV_USE_PERF_MONITOR=n
CONFIG_LV_USE_MEM_MONITOR=n
CONFIG_LV_USE_FREETYPE=n
CONFIG_ESP_LVGL_ADAPTER_ENABLE_FREETYPE=n
CONFIG_ESP_LVGL_ADAPTER_ENABLE_DECODER=n
CONFIG_ESP_LVGL_ADAPTER_ENABLE_FS=n
CONFIG_LV_FONT_MONTSERRAT_20=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=32768
CONFIG_ESP_LVGL_ADAPTER_ROTATE_IMAGE_CACHE_SIZE_KB=0
'''
    # Stock LVGL's PPA draw unit supports a single software draw unit. This is
    # a separate tuned official baseline, never silently mixed into native.
    sdkconfig += 'CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=' + ('1' if args.stock_ppa else '2') + '\n'
    sdkconfig += 'CONFIG_ESP_LVGL_ADAPTER_ROTATE_IN_DRAW=' + ('y' if native else 'n') + '\n'
    sdkconfig += 'CONFIG_LV_USE_MATRIX=' + ('y' if native else 'n') + '\n'
    sdkconfig += 'CONFIG_LV_DRAW_TRANSFORM_USE_MATRIX=' + ('y' if native else 'n') + '\n'
    # Supply it as the LAST defaults file. Regenerate sdkconfig when switching
    # variants; appending defaults does not override an old sdkconfig.
    (args.app / 'sdkconfig.ab').write_text(sdkconfig)
    metadata = {
        'protocol': 9, 'board': args.board, 'variant': variant, 'source_commit': args.source_sha,
        'source_tree': args.source_tree, 'rotation': rotation,
        'logical_size': [width, height], 'panel_framebuffers': 3,
        'tear_mode': 'DOUBLE_DIRECT' if native else 'TRIPLE_PARTIAL',
        'batch_limit': args.batch_limit if native else None,
        'software_draw_units': 1 if args.stock_ppa else 2,
        'stock_ppa_requested': args.stock_ppa,
        'rounds': args.rounds, 'warmup_frames': args.warmup, 'measured_frames': args.frames,
        'dashboard_full_redraw': args.dashboard_full,
        'dpi_scanout_probe_required': args.probe,
        'hardware_measurement': 'pending', 'sensor_capture': False,
        'sync_modes': ['eager', 'deferred'], 'timer_policy': 'automatic', 'paired_order': 'alternating by round, eager first on odd rounds',
        'diagnostic_windows': {'timings': 'production trace only; drain and closing lock separate',
                               'refresh_events': 'trace plus drain and closing lock; inclusive event durations',
                               'costs': 'trace plus drain and closing lock; inclusive, nested category times must not be added'},
        'v7_comparison': 'V9 compares eager/deferred empty-sync policies in the same native engine with automatic refresh retained; keep V7/V8 hardware captures separate',
        'engine_policy': 'per-display empty DIRECT sync deferral; normal automatic refresh remains active in both modes',
        'source_frames': 'four PSRAM RGB565 banks precomputed outside timed intervals',
        'immutable_wallpaper_sha256': hashlib.sha256(pixels).hexdigest(),
        'fixture_sha256': {name: hashlib.sha256((HERE / name).read_bytes()).hexdigest()
                           for name in ('ab_main.c', 'ab_scanout_probe.c', 'ab_scanout_probe.h', 'ab_cost_probe.c', 'ab_cost_probe.h', 'patch_dpi_probe.py')},
        'comparison_rule': 'within one board/firmware: identical native DOUBLE_DIRECT engine, input trace and hardware settings; only per-display empty-sync policy differs; normal automatic refresh allowed in both modes',
    }
    (args.app / 'ab-benchmark.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(json.dumps(metadata, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--board', choices=['tab5', 'waveshare-7b'], required=True)
    parser.add_argument('--variant', choices=['native'], required=True)
    parser.add_argument('--batch-limit', type=int, choices=[64, 128], default=64)
    parser.add_argument('--source-sha', required=True)
    parser.add_argument('--source-tree', required=True)
    parser.add_argument('--probe', action='store_true')
    parser.add_argument('--stock-ppa', action='store_true', help='official only: request stock PPA drawing with one software draw unit')
    parser.add_argument('--dashboard-full', action='store_true')
    parser.add_argument('--frames', type=int, default=64)
    parser.add_argument('--warmup', type=int, default=16)
    parser.add_argument('--rounds', type=int, default=2)
    args = parser.parse_args()
    if not 16 <= args.frames <= 4096 or not 1 <= args.warmup <= 256 or not 1 <= args.rounds <= 10:
        parser.error('frames/warmup/rounds are outside the bounded benchmark range')
    install(args)
