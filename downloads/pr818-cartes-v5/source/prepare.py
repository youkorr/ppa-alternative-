# SPDX-FileCopyrightText: 2026 youkorr
# SPDX-License-Identifier: MIT
"""Prepare the same hardware benchmark against the two pinned PR818 revisions."""
import argparse
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tarfile

BEFORE = 'ba1ed66c40c840cd1b2dbe9062760c2c86262b5a'
AFTER = '7965432ed3220ab29a2b44c84d78f341d4185224'
AFTER_V2 = '9db972e7008d4c5adab0fde59dc3b0b1343dac4c'
FIXTURES = '6cf0eed1277032489fd0281890b9a326c93f063f'
ADAPTER = 'components/display/tools/esp_lvgl_adapter'
HERE = Path(__file__).resolve().parent


def archive(repo, ref, paths, out):
    data = subprocess.check_output(['git', '-C', str(repo), 'archive', ref, *paths])
    with tarfile.open(fileobj=io.BytesIO(data)) as source:
        source.extractall(out, filter='data')


def prepare(args):
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    ref = {'before': BEFORE, 'after': AFTER, 'after_v2': AFTER_V2}[args.revision]
    archive(args.repo, ref, [ADAPTER, 'components/display/lcd/esp_lcd_ek79007', 'components/button'], out)
    fixtures = out / 'fixtures'
    fixtures.mkdir(exist_ok=True)
    archive(args.fixtures, FIXTURES, ['espressif'], fixtures)
    app = out / ADAPTER / 'test_apps/lvgl9'
    board_dir = 'tab5' if args.board == 'tab5' else 'waveshare-7b'
    angle = 270 if args.board == 'tab5' else 180
    subprocess.run([sys.executable, str(fixtures / 'espressif' / board_dir / 'prepare.py'),
                    str(app), 'rotate_in_draw', str(angle)], check=True)
    main = app / 'main/test_esp_lvgl_adapter_features.c'
    text = main.read_text()
    at = text.index('    /* ', text.index('void app_main(void)'))
    # Keep the original one-time display warm-up, then run only this benchmark.
    menu = text.rindex('    unity_run_menu();')
    start = text.index('    unity_run_test_by_name(', at)
    text = text[:start] + ('    ESP_LOGI(TAG, "PR818_READY: automatic benchmark starts in 10 seconds");\n'
                           '    vTaskDelay(pdMS_TO_TICKS(10000));\n'
                           '    unity_run_test_by_name("PR818 board comparison");\n'
                           '    ESP_LOGI(TAG, "PR818_DONE: save every PR818_RESULT line; reset for another run");\n') + text[menu:]
    text = text.replace('void app_main(void)\n', '#include "board_bench.inc.c"\n\nvoid app_main(void)\n', 1)
    if args.revision == 'after_v2':
        text = text.replace('    unity_run_test_by_name("PR818 board comparison");',
                            '    unity_run_test_by_name("PR818 board comparison");\n'
                            '    for (int i = 0; i < 16; i++)\n'
                            '        unity_run_test_by_name("adapter render threads can be stopped repeatedly");\n'
                            '    ESP_LOGI(TAG, "PR818_RESTART_OK count=16");', 1)
    main.write_text(text)
    (app / 'main/board_bench.inc.c').write_text((HERE / 'board_bench.inc.c').read_text())
    w, h = (1280, 720) if args.board == 'tab5' else (1024, 600)
    header = (f'#define BOARD_NAME "{args.board}"\n#define BOARD_REVISION "{args.revision}"\n'
              f'#define BOARD_COMMIT "{ref}"\n#define BENCH_HAS_CACHE {int(args.revision != "before")}\n'
              f'#define BOARD_WIDTH {w}\n#define BOARD_HEIGHT {h}\n')
    (app / 'main/board_bench_config.h').write_text(header)
    pixels = bytearray(w * h * 2)
    for y in range(h):
        for x in range(w):
            r, g, b = x * 31 // w, y * 63 // h, ((x // 64) ^ (y // 64)) & 31
            struct.pack_into('<H', pixels, (y * w + x) * 2, r << 11 | g << 5 | b)
    (app / 'main/board_image.rgb565').write_bytes(pixels)
    (app / 'main/CMakeLists.txt').write_text('idf_component_register(\n'
        '    SRCS "test_esp_lvgl_adapter_features.c"\n'
        '    INCLUDE_DIRS "."\n    REQUIRES hw_init unity esp_timer\n'
        '    EMBED_FILES "board_image.rgb565"\n)\n')
    # No network, filesystem or font/image-decoder assets are needed by the bench.
    # The application image includes the immutable RGB565 test background.
    (app / 'partitions.csv').write_text('# Name, Type, SubType, Offset, Size, Flags\n'
        'nvs, data, nvs, , 0x6000,\nphy_init, data, phy, , 0x1000,\n'
        'factory, app, factory, , 0x600000,\n')
    config = (app / 'sdkconfig.defaults').read_text() + '\n' + (app / 'sdkconfig.defaults.esp32p4').read_text()
    config += '\n' + (HERE / 'sdkconfig.common').read_text()
    config += '\nCONFIG_ESPTOOLPY_FLASHSIZE_' + ('16MB' if args.board == 'tab5' else '32MB') + '=y\n'
    (app / 'sdkconfig.bench').write_text(config)
    # Both boards use the same dependency set, allowing one shared build tree.
    manifest = app / 'components/hw_init/idf_component.yml'
    deps = manifest.read_text()
    if args.board != 'tab5':
        deps += '\n  espressif/esp_lcd_ili9881c:\n    version: "^1"\n'
        deps += '  espressif/esp_io_expander_pi4ioe5v6408:\n    version: "^1"\n'
    manifest.write_text(deps)
    meta = {'board': args.board, 'revision': args.revision, 'commit': ref,
            'fixtures_commit': FIXTURES, 'rotation': angle, 'logical_size': [w, h],
            'flash_size': '16MB' if args.board == 'tab5' else '32MB',
            'cpu_mhz': 360, 'psram_mhz': 200, 'l2_cache_kib': 128,
            'cache_budget_kib': 4096 if args.revision != 'before' else 0,
            'sensor_capture': False}
    (out / 'benchmark.json').write_text(json.dumps(meta, indent=2) + '\n')
    print(app)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--fixtures', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--board', choices=['tab5', 'waveshare-7b'], required=True)
    parser.add_argument('--revision', choices=['before', 'after', 'after_v2'], required=True)
    prepare(parser.parse_args())
