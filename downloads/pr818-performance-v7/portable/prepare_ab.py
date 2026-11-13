#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 youkorr
# SPDX-License-Identifier: MIT
"""Export an isolated, pinned ESP32-P4 A/B project; never alter the source repo.

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
CANDIDATE = '79b87e9e8703e85e66812850414428c4cd978865'
CANDIDATE_TREE = '765e2a516a3a42c91d0145150e35e9c5dc41293c'
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
    native = args.variant.startswith('native')
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
               '--batch-limit', '128' if args.variant == 'native128' else '64',
               '--source-sha', CANDIDATE if native else OFFICIAL,
               '--source-tree', CANDIDATE_TREE if native else official_tree, '--probe']
    if args.variant == 'official_ppa1':
        install.append('--stock-ppa')
    run(install, stdout=subprocess.DEVNULL)
    run([sys.executable, args.fixtures / 'patch_dpi_probe.py', '--idf', args.idf,
         '--out', app / 'components/esp_lcd'], stdout=subprocess.DEVNULL)
    meta = {
        'board': args.board, 'variant': args.variant, 'official_commit': OFFICIAL,
        'candidate_commit': CANDIDATE if native else None,
        'source_tree': CANDIDATE_TREE if native else official_tree,
        'candidate_delivery': 'official commit plus supplied patch; integrated commit may be local only',
        'source_component_files_before_fixture_overlay': component_fingerprints,
        'candidate_patch_sha256': hashlib.sha256(args.patch.read_bytes()).hexdigest() if native else None,
        'flash_size': flash, 'merged_flash_address': '0x0', 'hardware_measurement': 'pending',
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
    parser.add_argument('--variant', choices=['official', 'official_ppa1', 'native64', 'native128'], required=True)
    arguments = parser.parse_args()
    if arguments.idf is None:
        parser.error('set IDF_PATH or supply --idf')
    prepare(arguments)
