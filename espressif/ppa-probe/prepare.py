#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 youkorr
# Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
"""Prepare the PPA probe (probe.c) for one board.

The firmware measures, on the board, where a camera picture's turn onto the
panel spends its time: the PPA from and to PSRAM and internal SRAM, the
copies that could move a band from SRAM to the frame buffer, and a turn done
in bands through SRAM against the straight turn lv_draw_ortho does today.

It is built the way the PR 818 A/B benchmark is (../pr818-ab), from the same
board files and the same performance profile (360 MHz, PSRAM at 200 MHz, a
128 KB L2 cache), so its figures sit beside that benchmark's. The adapter is
there only because the board files are written against it; nothing in the
probe calls it, nor LVGL.

Needs git, Python 3.12 and ESP-IDF v5.5.6.
"""
import argparse
import importlib.util
from pathlib import Path
import shutil

HERE = Path(__file__).resolve().parent
AB_DIR = HERE.parent / 'pr818-ab'
_spec = importlib.util.spec_from_file_location('ab_prepare', AB_DIR / 'prepare.py')
ab = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ab)

# The picture each board's camera YAML turns, and where it lands. Tab5: the
# SC2356's 1280x720, turned 270 by the PPA (LVGL's 270 is the PPA's 270, as
# lv_draw_ortho_ppa.c's ppa_angle says) onto the 720x1280 panel. Waveshare 7B:
# the middle 1024x600 of the OV5647's 1280x960, turned 180.
GEOMETRY = {
    'tab5': dict(src=(1280, 720), block=(0, 0, 1280, 720), out=(720, 1280), angle=270),
    'waveshare-7b': dict(src=(1280, 960), block=(128, 180, 1024, 600), out=(1024, 600), angle=180),
}


def prepare(args):
    out = args.out.resolve()
    if out.exists() and any(out.iterdir()):
        raise ValueError('--out must be new or empty')
    out.mkdir(parents=True, exist_ok=True)
    sha, _tree = ab.export(args.repo.resolve(), args.ref, out)
    geo = GEOMETRY[args.board]
    flash = ab.SIZES[args.board][3]
    app = out / ab.ADAPTER / 'test_apps/lvgl9'

    hw = app / 'components/hw_init'
    shutil.rmtree(hw)
    shutil.copytree(AB_DIR / 'boards' / args.board / 'hw_init', hw)
    (app / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.5)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ppa_probe)
''')
    camera = args.video is not None
    manifest = '''dependencies:
  idf:
    version: ">=5.5.5"
  lvgl/lvgl:
    version: "9.6.0~1"
    public: true
  hw_init:
    path: "../components/hw_init"
  espressif/button:
    version: "4.*"
    override_path: "../../../../../../button"
'''
    if camera:
        video = args.video.resolve()
        for name in ('esp_video', 'esp_cam_sensor', 'esp_ipa', 'esp_sccb_intf'):
            if not (video / name / 'idf_component.yml').is_file():
                raise ValueError(f'--video: {video / name} is not an esp-video-components component')
            manifest += f'  espressif/{name}:\n    version: "*"\n    override_path: "{video / name}"\n'
    main = app / 'main'
    for f in main.iterdir():
        if f.is_file():
            f.unlink()
    (main / 'idf_component.yml').write_text(manifest)
    (app / 'partitions.csv').write_text('''# Name, Type, SubType, Offset, Size, Flags
nvs, data, nvs, , 0x6000,
phy_init, data, phy, , 0x1000,
factory, app, factory, , 0x600000,
''')
    (app / 'sdkconfig.bench').write_text((AB_DIR / 'portable/sdkconfig.common').read_text() +
                                         '\nCONFIG_ESPTOOLPY_FLASHSIZE_' + flash + '=y\n')
    (scl, sda), camera_sdkconfig = ab.CAMERAS[args.board]
    (app / 'sdkconfig.probe').write_text('''# The A/B benchmark's performance profile, so the figures compare.
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
CONFIG_ESP_MAIN_TASK_STACK_SIZE=16384
''' + ('''CONFIG_ESP_VIDEO_ENABLE_MIPI_CSI_VIDEO_DEVICE=y
CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER=y
''' + camera_sdkconfig if camera else ''))

    shutil.copy2(HERE / 'probe.c', main / 'probe.c')
    if camera:
        for name in ('ab_camera.c', 'ab_camera.h'):
            shutil.copy2(AB_DIR / 'fixtures' / name, main / name)
        # ab_camera.c takes the Waveshare's camera pins from here.
        (main / 'ab_bench_config.h').write_text(f'#define AB_CAMERA_SCL ({scl})\n#define AB_CAMERA_SDA ({sda})\n')
    sx, sy = geo['src']
    bx, by, bw, bh = geo['block']
    ow, oh = geo['out']
    if (bw, bh) != ((ow, oh) if geo['angle'] in (0, 180) else (oh, ow)):
        raise ValueError(f'{args.board}: a {bw}x{bh} block turned {geo["angle"]} is not {ow}x{oh}')
    config = {
        'PROBE_BOARD': f'"{args.board}"', 'PROBE_SOURCE_COMMIT': f'"{sha}"',
        'PROBE_SRC_W': f'{sx}u', 'PROBE_SRC_H': f'{sy}u',
        'PROBE_BLOCK_X': f'{bx}u', 'PROBE_BLOCK_Y': f'{by}u', 'PROBE_BLOCK_W': f'{bw}u', 'PROBE_BLOCK_H': f'{bh}u',
        'PROBE_OUT_W': f'{ow}u', 'PROBE_OUT_H': f'{oh}u',
        'PROBE_ANGLE': f'PPA_SRM_ROTATION_ANGLE_{geo["angle"]}', 'PROBE_HAS_CAMERA': str(int(camera)),
    }
    (main / 'probe_config.h').write_text('#pragma once\n' + ''.join(f'#define {k} {v}\n' for k, v in config.items()))
    (main / 'CMakeLists.txt').write_text('''idf_component_register(
    SRCS "probe.c"''' + (' "ab_camera.c"' if camera else '') + '''
    INCLUDE_DIRS "."
    REQUIRES hw_init esp_timer esp_mm esp_driver_ppa esp_lcd esp_hw_support esp_rom hal''' +
                                          (' esp_driver_ledc' if camera else '') + '''
)
# esp_async_fbcpy is the DMA2D copy the DSI driver itself uses to put a buffer
# in its frame buffer; its header is private to esp_lcd, the code is linked in.
target_include_directories(${COMPONENT_LIB} PRIVATE "$ENV{IDF_PATH}/components/esp_lcd/priv_include")
''')
    print(app)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--repo', type=Path, required=True, help='an esp-iot-solution checkout holding --ref')
    p.add_argument('--ref', required=True, help='the commit the board files\' adapter comes from')
    p.add_argument('--board', choices=sorted(GEOMETRY), required=True)
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--video', type=Path, help='an espressif/esp-video-components checkout: adds the camera condition')
    prepare(p.parse_args())
