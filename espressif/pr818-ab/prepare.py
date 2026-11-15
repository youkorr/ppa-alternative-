#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 youkorr
# Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
"""Prepare the A/B benchmark of esp-iot-solution PR 818 for one board.

The same benchmark as the V9 kit (fixtures/ and boards/, from the
test/pr818-card-firmwares-20261112 branch): a dashboard, a picture changing
every frame, a picture in the flash with no cache and with one -- 64 frames
after 16 to warm up, twice, each scene in two modes alternately. Built from
any commit of esp-iot-solution, so the adapter as Espressif ships it and the
PR's are measured by the same firmware code, on the same day:

  --variant official   the adapter at --ref, drawn upright and turned by the
                       adapter (TRIPLE_PARTIAL), as Espressif ships it
  --variant native     the adapter at --ref drawn in the panel's orientation
                       (ROTATE_IN_DRAW, DOUBLE_DIRECT)

The two modes are V9's, mapped onto what the PR does now: "eager" is LVGL
keeping its frame buffers in step itself, "deferred" is lv_draw_ortho doing
it (lv_draw_ortho_take_sync) -- the V9 kit patched LVGL for this instead. On
the official adapter both modes are the same.

Needs git, Python 3.12 and ESP-IDF v5.5.6 ($IDF_PATH), for the scanout probe.
"""
import argparse
import io
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tarfile

HERE = Path(__file__).resolve().parent
ADAPTER = Path('components/display/tools/esp_lvgl_adapter')
SIZES = {'tab5': (1280, 720, 270, '16MB'), 'waveshare-7b': (1024, 600, 180, '32MB')}

# The camera scene (--video): each board's camera as the household's working
# ESPHome configurations run it. The Tab5's SC2356 is Espressif's SC202CS
# driver (the same chip, PID 0xeb52) and answers on the bus the display opened
# (hw_camera_prepare), so it has no pins here; the Waveshare's OV5647 is on
# I2C SCL GPIO8, SDA GPIO7.
CAMERAS = {
    'tab5': ((-1, -1), '''CONFIG_CAMERA_SC202CS=y
CONFIG_CAMERA_SC202CS_AUTO_DETECT_MIPI_INTERFACE_SENSOR=y
CONFIG_CAMERA_SC202CS_MIPI_RAW8_1280X720_30FPS=y
CONFIG_CAMERA_SC202CS_MIPI_DEFAULT_FMT_RAW8_1280X720_30FPS=y
'''),
    'waveshare-7b': ((8, 7), '''CONFIG_CAMERA_OV5647=y
CONFIG_CAMERA_OV5647_AUTO_DETECT_MIPI_INTERFACE_SENSOR=y
CONFIG_CAMERA_OV5647_MIPI_RAW10_1280X960_BINNING_45FPS=y
CONFIG_CAMERA_OV5647_MIPI_DEFAULT_FMT_RAW10_1280X960_BINNING_45FPS=y
'''),
}


def run(cmd, **kw):
    return subprocess.run([str(c) for c in cmd], check=True, **kw)


def export(repo, ref, out):
    archive = subprocess.check_output(['git', '-C', str(repo), 'archive', ref, str(ADAPTER),
                                       'components/display/lcd/esp_lcd_ek79007', 'components/button'])
    with tarfile.open(fileobj=io.BytesIO(archive)) as tf:
        try:
            tf.extractall(out, filter='data')
        except TypeError:   # a Python without extraction filters: our own archive
            tf.extractall(out)
    sha = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', ref + '^{commit}'], text=True).strip()
    tree = subprocess.check_output(['git', '-C', str(repo), 'rev-parse', ref + '^{tree}'], text=True).strip()
    return sha, tree


# The V9 benchmark switches LVGL's sync policy through a setter its patched
# LVGL had. Here the same two modes are lv_draw_ortho's takeover on and off.
SYNC_SHIM = '''
/* "deferred": lv_draw_ortho keeps the frame buffers in step
 * (lv_draw_ortho_take_sync): what LVGL would copy is written down, and only
 * what the next frame does not draw is copied, by the PPA. "eager": LVGL
 * copies, as without it. The same on the official adapter. */
static bool s_sync_taken;
static void ab_set_sync(bool take)
{
#if AB_HAS_NATIVE
    if (take) {
        s_sync_taken = lv_draw_ortho_take_sync(s_display);
    } else {
        lv_draw_ortho_release_sync(s_display);
        s_sync_taken = false;
    }
#else
    (void)take;
    s_sync_taken = false;
#endif
}
'''


def patch_main(main_c):
    s = main_c.read_text()

    def rep(old, new):
        nonlocal s
        if s.count(old) != 1:
            raise ValueError(f'ab_main.c: {old.strip()[:60]!r} found {s.count(old)} times')
        s = s.replace(old, new)

    rep('static uint8_t *s_bank[4];\n', 'static uint8_t *s_bank[4];\n' + SYNC_SHIM)
    rep('    lv_display_set_defer_empty_direct_sync(s_display, defer_sync);\n', '    ab_set_sync(defer_sync);\n')
    rep('    const bool policy_enabled = lv_display_get_defer_empty_direct_sync(s_display);\n',
        '    const bool policy_enabled = s_sync_taken;\n')
    main_c.write_text(s)


def prepare(args):
    out = args.out.resolve()
    if out.exists() and any(out.iterdir()):
        raise ValueError('--out must be new or empty')
    out.mkdir(parents=True, exist_ok=True)
    sha, tree = export(args.repo.resolve(), args.ref, out)
    native = args.variant == 'native'
    width, height, rotation, flash = SIZES[args.board]
    app = out / ADAPTER / 'test_apps/lvgl9'

    hw = app / 'components/hw_init'
    shutil.rmtree(hw)
    shutil.copytree(HERE / 'boards' / args.board / 'hw_init', hw)
    (app / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.5)
set(_unit_test_components "$ENV{IDF_PATH}/tools/unit-test-app/components")
if(EXISTS "${_unit_test_components}")
    set(EXTRA_COMPONENT_DIRS "${_unit_test_components}")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(test_esp_lvgl_adapter_lvgl9)
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
        for name in ('esp_video', 'esp_cam_sensor', 'esp_ipa'):
            if not (video / name / 'idf_component.yml').is_file():
                raise ValueError(f'--video: {video / name} is not an esp-video-components component')
            manifest += f'  espressif/{name}:\n    version: "*"\n    override_path: "{video / name}"\n'
    (app / 'main/idf_component.yml').write_text(manifest)
    (app / 'partitions.csv').write_text('''# Name, Type, SubType, Offset, Size, Flags
nvs, data, nvs, , 0x6000,
phy_init, data, phy, , 0x1000,
factory, app, factory, , 0x600000,
''')
    (app / 'sdkconfig.bench').write_text((HERE / 'portable/sdkconfig.common').read_text() +
                                         '\nCONFIG_ESPTOOLPY_FLASHSIZE_' + flash + '=y\n')

    main = app / 'main'
    for f in main.iterdir():
        if f.is_file() and f.suffix in ('.c', '.h', '.txt'):
            f.unlink()
    for name in ('ab_main.c', 'ab_scanout_probe.c', 'ab_scanout_probe.h', 'ab_cost_probe.c', 'ab_cost_probe.h') + \
            (('ab_camera.c', 'ab_camera.h') if camera else ()):
        shutil.copy2(HERE / 'fixtures' / name, main / name)
    patch_main(main / 'ab_main.c')
    variant = 'native64' if native else 'official'
    header = {
        'AB_PROTOCOL_VERSION': 9, 'AB_BOARD_NAME': args.board, 'AB_VARIANT': variant,
        'AB_SOURCE_COMMIT': sha, 'AB_SOURCE_TREE': tree, 'AB_WIDTH': width, 'AB_HEIGHT': height,
        'AB_BATCH_LIMIT': 64 if native else 0, 'AB_HAS_NATIVE': int(native), 'AB_HAS_DPI_PROBE': 1,
        'AB_STOCK_PPA': 0, 'AB_DASHBOARD_FULL': 0, 'AB_FRAMES': 64, 'AB_ROUNDS': 2, 'AB_WARMUP': 16,
        'AB_HAS_CAMERA': int(camera),
    }
    config = ''.join(f'#define {k} ' + (f'"{v}"' if isinstance(v, str) else f'{v}u') + '\n' for k, v in header.items())
    config += f'#define AB_ROTATION ESP_LV_ADAPTER_ROTATE_{rotation}\n'
    (scl, sda), camera_sdkconfig = CAMERAS[args.board]
    config += f'#define AB_CAMERA_SCL ({scl})\n#define AB_CAMERA_SDA ({sda})\n'
    config += '#define AB_TEAR_MODE ESP_LV_ADAPTER_TEAR_AVOID_MODE_' + ('DOUBLE_DIRECT' if native else 'TRIPLE_PARTIAL') + '\n'
    (main / 'ab_bench_config.h').write_text(config)
    # The fixed picture, the V9 kit's own: a gradient with a checkerboard.
    pixels = bytearray(width * height * 2)
    for y in range(height):
        for x in range(width):
            r, g, b = x * 31 // width, y * 63 // height, ((x // 48) ^ (y // 48)) & 31
            struct.pack_into('<H', pixels, (y * width + x) * 2, r << 11 | g << 5 | b)
    (main / 'ab_wallpaper.rgb565').write_bytes(pixels)
    cmake = '''idf_component_register(
    SRCS "ab_main.c" "ab_scanout_probe.c" "ab_cost_probe.c"''' + (' "ab_camera.c"' if camera else '') + '''
    INCLUDE_DIRS "."
    REQUIRES hw_init esp_timer esp_mm esp_driver_ppa''' + (' esp_driver_ledc' if camera else '') + '''
    EMBED_FILES "ab_wallpaper.rgb565"
)
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
target_compile_definitions(${_ab_adapter_lib} PRIVATE LV_DRAW_ORTHO_MAX_BATCH=64)
'''
    (main / 'CMakeLists.txt').write_text(cmake)
    (app / 'sdkconfig.ab').write_text('''# A/B benchmark profile: the V9 kit's, identical for both variants.
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
CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=2
# AB_CPU: each core's idle time, from FreeRTOS's run-time counters.
CONFIG_FREERTOS_USE_TRACE_FACILITY=y
CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y
''' + ''.join(f'{k}={"y" if native else "n"}\n' for k in (
        'CONFIG_ESP_LVGL_ADAPTER_ROTATE_IN_DRAW', 'CONFIG_LV_USE_MATRIX', 'CONFIG_LV_DRAW_TRANSFORM_USE_MATRIX')) +
        ('''# The camera scene: MIPI-CSI through the ISP, with its automatic exposure
# and white balance (esp_ipa), identical for both variants.
CONFIG_ESP_VIDEO_ENABLE_MIPI_CSI_VIDEO_DEVICE=y
CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER=y
''' + camera_sdkconfig if camera else ''))
    if native:
        source = out / ADAPTER / 'src/display/rotate_in_draw/lv_draw_ortho_ppa.c'
        run([sys.executable, HERE / 'fixtures/patch_native_wait_probe.py', '--source', source],
            stdout=subprocess.DEVNULL)
    run([sys.executable, HERE / 'fixtures/patch_dpi_probe.py', '--idf', args.idf, '--out', app / 'components/esp_lcd'],
        stdout=subprocess.DEVNULL)
    print(app)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--repo', type=Path, required=True, help='an esp-iot-solution checkout holding --ref')
    p.add_argument('--ref', required=True, help='the commit, branch or tag to build the adapter from')
    p.add_argument('--variant', choices=['official', 'native'], required=True)
    p.add_argument('--board', choices=sorted(SIZES), required=True)
    p.add_argument('--idf', type=Path, default=Path(os.environ.get('IDF_PATH', '')))
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--video', type=Path, help='an espressif/esp-video-components checkout: adds the camera scene')
    prepare(p.parse_args())
