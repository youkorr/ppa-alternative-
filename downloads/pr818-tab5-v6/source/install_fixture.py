#!/usr/bin/env python3
"""Install V6 into an already prepared M5Stack Tab5 LVGL9 test application.

Does not edit the IDF framework or apply the rendering correction. The caller
must stage the corrected adapter and LVGL per-display capability patch first.
"""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re
import shutil

NAMES = ['control', 'image-rot30', 'image-rot90', 'image-scale', 'image-rot30-scale',
         'object-rot30', 'object-rot90', 'object-scale', 'object-rot30-scale',
         'parent-rot30', 'parent-scale', 'nested-rotations', 'nested-scales',
         'layered-object-rot30', 'layered-parent-child', 'clipped-object-rot30']


def install(app, oracle, reference_version='9.6.0'):
    here = Path(__file__).resolve().parent
    main = app / 'main/test_esp_lvgl_adapter_features.c'
    text = main.read_text()
    rotation = re.compile(r'static esp_lv_adapter_rotation_t get_configured_rotation\(void\)\n\{.*?\n\}', re.S)
    assert len(rotation.findall(text)) == 1
    existing = re.compile(r'static esp_lv_adapter_rotation_t s_v6_rotation = ESP_LV_ADAPTER_ROTATE_\d+;')
    if existing.search(text):
        text = existing.sub('static esp_lv_adapter_rotation_t s_v6_rotation = ESP_LV_ADAPTER_ROTATE_270;', text, count=1)
        declaration = ''
    else:
        declaration = 'static esp_lv_adapter_rotation_t s_v6_rotation = ESP_LV_ADAPTER_ROTATE_270;\n\n'
    text = rotation.sub(declaration +
                        'static esp_lv_adapter_rotation_t get_configured_rotation(void)\n{\n'
                        '    return s_v6_rotation;\n}', text, count=1)
    text = text.replace('#include "board_bench.inc.c"\n', '')
    if '#include "transform_bench.inc.c"' in text:
        text = text[:text.index('#include "transform_bench.inc.c"')]
    elif '#include "memory_bench.inc.c"' in text:
        text = text[:text.index('#include "memory_bench.inc.c"')]
    else:
        assert 'void app_main(void)' in text
        text = text[:text.rindex('void app_main(void)')]
    text += '#include "transform_bench.inc.c"\n'
    main.write_text(text)
    shutil.copy2(here / 'transform_bench.inc.c', app / 'main/transform_bench.inc.c')
    pixels = bytearray()
    hashes = {}
    for name in NAMES:
        source = oracle / f'{name}-320x480-16-r0-direct.bin.gz'
        raw = gzip.decompress(source.read_bytes())
        assert len(raw) == 320 * 480 * 2
        hashes[name] = hashlib.sha256(raw).hexdigest()
        for y in range(160, 360):
            pixels += raw[(y * 320 + 40) * 2:(y * 320 + 280) * 2]
    assert len(pixels) == 16 * 240 * 200 * 2
    (app / 'main/v6_reference.rgb565').write_bytes(pixels)
    (app / 'main/v6_reference_config.h').write_text('#define V6_REFERENCE_LVGL ' + json.dumps(reference_version) + '\n')
    (app / 'main/v6_board_config.h').write_text(
        '#define V6_BOARD_NAME "tab5"\n'
        '#define V6_BOARD_LABEL "M5Stack Tab5"\n'
        '#define V6_ROTATIONS {ESP_LV_ADAPTER_ROTATE_270, ESP_LV_ADAPTER_ROTATE_90, '
        'ESP_LV_ADAPTER_ROTATE_180, ESP_LV_ADAPTER_ROTATE_0}\n'
        '#define V6_FINAL_ROTATION ESP_LV_ADAPTER_ROTATE_270\n')
    (app / 'main/CMakeLists.txt').write_text('idf_component_register(\n'
        '    SRCS "test_esp_lvgl_adapter_features.c"\n'
        '    INCLUDE_DIRS "."\n'
        '    REQUIRES hw_init unity esp_timer\n'
        '    EMBED_FILES "v6_reference.rgb565"\n)\n')
    (app / 'main/v6-reference-manifest.json').write_text(json.dumps({
        'board': 'tab5', 'board_label': 'M5Stack Tab5',
        'native_size': [720, 1280], 'default_logical_size': [1280, 720],
        'rotations': [270, 90, 180, 0], 'final_rotation': 270,
        'reference_lvgl': reference_version, 'matrix_object_transforms': False,
        'source_native_size': [320, 480], 'source_screen_rotation': 0,
        'crop': [40, 160, 240, 200], 'format': 'RGB565 little-endian',
        'scenes': NAMES, 'source_uncompressed_sha256': hashes,
        'embedded_asset_sha256': hashlib.sha256(pixels).hexdigest(),
        'automatic_pixel_equality': False,
    }, indent=2) + '\n')
    print('Installed V6 fixture:', app)
    print('Reference asset:', len(pixels), 'bytes', hashlib.sha256(pixels).hexdigest())


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, required=True)
    parser.add_argument('--oracle', type=Path, required=True)
    parser.add_argument('--reference-version', default='9.6.0')
    args = parser.parse_args()
    install(args.app, args.oracle, args.reference_version)
