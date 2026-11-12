#!/usr/bin/env python3
"""Reconstruct the dedicated Tab5 V6 application from pinned public sources.

The output must be empty. First reconstruct the successful Tab5 V5.2 board
setup, then apply only the additional three-file V6 object-transform delta.
No already-built firmware or private workspace path is required.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import subprocess
from types import SimpleNamespace

import prepare_v5_2
import install_fixture

HERE = Path(__file__).resolve().parent
ADAPTER = Path('components/display/tools/esp_lvgl_adapter')


def prepare(args):
    args.repo = args.repo.resolve()
    args.fixtures = args.fixtures.resolve()
    args.out = args.out.resolve()
    args.idf = args.idf.resolve()
    if args.out.exists() and any(args.out.iterdir()):
        raise ValueError('Use an empty output directory for this reconstruction')
    app = prepare_v5_2.make(SimpleNamespace(
        repo=args.repo, fixtures=args.fixtures, out=args.out,
        idf=args.idf, board='tab5'))

    delta = HERE / 'candidate-v6-object-transforms.patch'
    subprocess.run(['git', 'apply', '--check', str(delta)], cwd=args.out, check=True)
    subprocess.run(['git', 'apply', str(delta)], cwd=args.out, check=True)

    reference_dir = args.out / 'reference-oracle'
    reference_dir.mkdir()
    references = json.loads((HERE / 'oracle-base64.json').read_text())
    expected = {name + '-320x480-16-r0-direct.bin.gz' for name in install_fixture.NAMES}
    if set(references) != expected:
        raise ValueError('The 16 frozen LVGL9.6.0 software reference frames are required')
    for name, data in references.items():
        (reference_dir / name).write_bytes(base64.b64decode(data, validate=True))
    install_fixture.install(app, reference_dir, '9.6.0')
    with (app / 'sdkconfig.bench').open('a') as output:
        output.write('\nCONFIG_LV_FONT_MONTSERRAT_14=y\nCONFIG_LV_FONT_MONTSERRAT_20=y\n')

    lock = (HERE / 'tab5-dependencies.lock.template').read_text()
    (app / 'dependencies.lock').write_text(lock.replace('${STAGE_ROOT}', str(args.out)))
    production = ['CMakeLists.txt', 'src/display/display_manager.c',
                  'src/display/rotate_in_draw/lv_draw_ortho.c',
                  'src/display/rotate_in_draw/lv_draw_ortho_ppa.c',
                  'tools/patch_lvgl_object_transforms.py', 'tools/patch_lvgl_os_cleanup.py',
                  'tools/patch_lvgl_thread_shutdown.py']
    metadata = {
        'board': 'tab5', 'board_label': 'M5Stack Tab5', 'revision': 'transforms_v6',
        'base_pr_head': '9db972e7008d4c5adab0fde59dc3b0b1343dac4c',
        'fixtures_commit': '6cf0eed1277032489fd0281890b9a326c93f063f',
        'flash_size': '16MB', 'cpu_mhz': 360, 'psram_mhz': 200, 'l2_cache_kib': 128,
        'rotation': 270, 'rotations': [270, 90, 180, 0], 'final_rotation': 270,
        'native_size': [720, 1280], 'logical_size': [1280, 720],
        'test_mode': 'object_transforms_visual_and_heap_integrity',
        'cycles': 2, 'scenes_per_rotation': 16, 'expected_scene_logs': 128,
        'hardware_validation': 'pending', 'automatic_hardware_pixel_equality': False,
        'software_object_transforms': 'per-display, explicit opt-in',
        'thread_shutdown_barrier': True, 'os_mutex_cleanup': True,
        'ppa_cache_prepare_max_bytes': 131072, 'sensor_capture': False,
        'reference_lvgl': '9.6.0',
        'baseline_v52_patch_sha256': hashlib.sha256((HERE / 'candidate-v5.patch').read_bytes()).hexdigest(),
        'object_transform_delta_sha256': hashlib.sha256(delta.read_bytes()).hexdigest(),
        'candidate_files_sha256': {
            name: hashlib.sha256((args.out / ADAPTER / name).read_bytes()).hexdigest()
            for name in production},
    }
    (args.out / 'benchmark.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print('Tab5 V6 staged:', app)
    return app


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, required=True,
                        help='Clone of youkorr/esp-iot-solution containing PR818 head9db972e')
    parser.add_argument('--fixtures', type=Path, required=True,
                        help='Clone of youkorr/ppa-alternative- containing fixture commit6cf0eed')
    parser.add_argument('--out', type=Path, required=True, help='Empty staging directory')
    parser.add_argument('--idf', type=Path, required=True, help='ESP-IDF v5.5.6 checkout')
    prepare(parser.parse_args())
