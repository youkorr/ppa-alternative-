#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 youkorr
# SPDX-License-Identifier: MIT
"""Export an isolated, pinned ESP32-P4 V9 diagnostic project.

The input repository and shared SDK remain unchanged. Each resulting binary
compares legacy and strict refresh-timer policies with identical cost probes.

Requires Python >=3.12, git, patch and ESP-IDF v5.5.6. The output must be empty;
use a separate output and managed_components directory for every variant.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile

OFFICIAL = 'da53cfaa90b63085ef21f721f7593b5e2747d448'
CANDIDATE = 'f8ce296abfe2f6cc1fa38fc5ef1928361f1dc3a7'
CANDIDATE_TREE = '88fec6fe91b499dfd9ab62face91a2673ff429a0'
ADAPTER = Path('components/display/tools/esp_lvgl_adapter')
HERE = Path(__file__).resolve().parent


def run(args, **kwargs):
    return subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def fingerprint_tree(root):
    return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob('*')) if p.is_file()}


def prepare(args):
    args.repo = args.repo.resolve()
    args.out = args.out.resolve()
    args.fixtures = args.fixtures.resolve()
    args.boards = args.boards.resolve()
    args.patch = args.patch.resolve()
    args.idf = args.idf.resolve()
    if args.out.exists() and (not args.out.is_dir() or any(args.out.iterdir())):
        raise ValueError('--out must be a new or empty private directory')
    if (args.idf / 'version.txt').read_text().strip() != 'v5.5.6':
        raise ValueError('Use ESP-IDF v5.5.6 for the fixed scanout probe')
    native = args.variant == 'native64'
    required_fixtures = ('install_ab.py', 'ab_main.c', 'ab_scanout_probe.c',
                         'ab_scanout_probe.h', 'ab_cost_probe.c', 'ab_cost_probe.h',
                         'patch_dpi_probe.py', 'patch_native_wait_probe.py')
    for name in required_fixtures:
        if not (args.fixtures / name).is_file():
            raise ValueError(f'missing V9 fixture: {name}')
    fixture_fingerprints = {name: hashlib.sha256((args.fixtures / name).read_bytes()).hexdigest()
                            for name in required_fixtures}
    # Export exact Git objects, independent of the caller's current branch or dirt.
    official_tree = subprocess.check_output(
        ['git', '-C', str(args.repo), 'rev-parse', OFFICIAL + '^{tree}'], text=True).strip()
    archive = subprocess.check_output([
        'git', '-C', str(args.repo), 'archive', OFFICIAL, str(ADAPTER),
        'components/display/lcd/esp_lcd_ek79007', 'components/button'])
    args.out.mkdir(parents=True, exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(archive)) as tf:
        tf.extractall(args.out, filter='data')
    if native:
        run(['patch', '-d', args.out, '-p1', '--batch', '--forward', '--input', args.patch],
            stdout=subprocess.DEVNULL)
    component_fingerprints = fingerprint_tree(args.out / ADAPTER)
    app = args.out / ADAPTER / 'test_apps/lvgl9'
    # Board files match the previously validated V6 panel setup. No touch/audio,
    # camera, Home Assistant network service or changing board settings are added.
    hw = app / 'components/hw_init'
    shutil.rmtree(hw)
    shutil.copytree(args.boards / args.board / 'hw_init', hw)
    (app / 'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.5)
set(_unit_test_components "$ENV{IDF_PATH}/tools/unit-test-app/components")
if(EXISTS "${_unit_test_components}")
    set(EXTRA_COMPONENT_DIRS "${_unit_test_components}")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(test_esp_lvgl_adapter_lvgl9)
''')
    (app / 'main/idf_component.yml').write_text('''dependencies:
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
''')
    (app / 'partitions.csv').write_text('''# Name, Type, SubType, Offset, Size, Flags
nvs, data, nvs, , 0x6000,
phy_init, data, phy, , 0x1000,
factory, app, factory, , 0x600000,
''')
    flash = '16MB' if args.board == 'tab5' else '32MB'
    base_config = (HERE / 'sdkconfig.common').read_text()
    (app / 'sdkconfig.bench').write_text(base_config + '\nCONFIG_ESPTOOLPY_FLASHSIZE_' + flash + '=y\n')
    install = [sys.executable, args.fixtures / 'install_ab.py', '--app', app,
               '--board', args.board, '--variant', 'native' if native else 'official',
               '--batch-limit', '64', '--frames', '64', '--rounds', '2', '--warmup', '16',
               '--source-sha', CANDIDATE if native else OFFICIAL,
               '--source-tree', CANDIDATE_TREE if native else official_tree, '--probe']
    run(install, stdout=subprocess.DEVNULL)
    installed = json.loads((app / 'ab-benchmark.json').read_text())
    if (installed.get('protocol') != 9 or installed.get('sync_modes') != ['eager', 'deferred']
            or installed.get('measured_frames') != 64 or installed.get('rounds') != 2
            or installed.get('software_draw_units') != 2
            or installed.get('variant') != args.variant):
        raise ValueError('installer did not produce the fixed V9 diagnostic protocol')
    native_wait_probe = None
    if native:
        source = args.out / ADAPTER / 'src/display/rotate_in_draw/lv_draw_ortho_ppa.c'
        native_wait_probe = json.loads(subprocess.check_output(
            [sys.executable, str(args.fixtures / 'patch_native_wait_probe.py'),
             '--source', str(source)], text=True))
        (app / 'native-wait-probe.json').write_text(json.dumps(native_wait_probe, indent=2) + '\n')
    run([sys.executable, args.fixtures / 'patch_dpi_probe.py', '--idf', args.idf,
         '--out', app / 'components/esp_lcd'], stdout=subprocess.DEVNULL)
    meta = {
        'protocol': 9, 'board': args.board, 'variant': args.variant, 'official_commit': OFFICIAL,
        'candidate_commit': CANDIDATE if native else None,
        'source_tree': CANDIDATE_TREE if native else official_tree,
        'candidate_delivery': 'official commit plus supplied patch; integrated commit may be local only',
        'source_component_files_before_fixture_overlay': component_fingerprints,
        'candidate_patch_sha256': hashlib.sha256(args.patch.read_bytes()).hexdigest() if native else None,
        'flash_size': flash, 'merged_flash_address': '0x0', 'hardware_measurement': 'pending',
        'merged_firmware_filename': f'{args.board}-diagnostics-v8-{args.variant}.bin',
        'sync_modes': ['eager', 'deferred'], 'measured_frames_per_mode': 64,
        'rounds': 2, 'warmup_frames': 16, 'fixture_sha256': fixture_fingerprints,
        'board_files_sha256': fingerprint_tree(args.boards / args.board / 'hw_init'),
        'native_wait_observer': native_wait_probe,
        'production_algorithm_changed': False,
        'timing_caveat': 'cost probes measure inclusive wall times and may perturb timing; compare matching V9 diagnostic variants and paired policies',
    }
    (args.out / 'portable-preparation.json').write_text(json.dumps(meta, indent=2) + '\n')
    print(app)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, required=True, help='esp-iot-solution repo containing pinned official commit')
    parser.add_argument('--fixtures', type=Path, default=HERE.parent / 'fixtures')
    parser.add_argument('--boards', type=Path, default=HERE.parent / 'boards')
    parser.add_argument('--patch', type=Path, default=HERE.parent / 'integration-on-0.7.8.patch')
    parser.add_argument('--idf', type=Path, default=Path(os.environ['IDF_PATH']) if os.environ.get('IDF_PATH') else None)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--board', choices=['tab5', 'waveshare-7b'], required=True)
    parser.add_argument('--variant', choices=['native64'], required=True)
    arguments = parser.parse_args()
    if arguments.idf is None:
        parser.error('set IDF_PATH or supply --idf')
    prepare(arguments)
