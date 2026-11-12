# SPDX-FileCopyrightText: 2026 youkorr
# SPDX-License-Identifier: MIT
"""Export the actual V5 flash segments and provenance for either card."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--stage', type=Path, required=True)
parser.add_argument('--build', type=Path, required=True)
parser.add_argument('--out', type=Path, required=True)
args = parser.parse_args()
app = args.stage / 'components/display/tools/esp_lvgl_adapter/test_apps/lvgl9'
metadata = json.loads((args.stage / 'benchmark.json').read_text())
board = metadata['board']
flasher = json.loads((args.build / 'flasher_args.json').read_text())
args.out.mkdir(parents=True, exist_ok=True)
image = args.out / f'{board}-after_v5_2.bin'
command = [sys.executable, '-m', 'esptool', '--chip', 'esp32p4', 'merge_bin', '-o', str(image),
           '--flash_mode', flasher['flash_settings']['flash_mode'],
           '--flash_freq', flasher['flash_settings']['flash_freq'],
           '--flash_size', flasher['flash_settings']['flash_size']]
for offset, filename in sorted(flasher['flash_files'].items(), key=lambda entry: int(entry[0], 0)):
    command.extend([offset, str((args.build / filename).resolve())])
subprocess.run(command, check=True)
config = (app / 'sdkconfig').read_text()
for expected in ('CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y', 'CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=360',
                 'CONFIG_CACHE_L2_CACHE_SIZE=0x20000', 'CONFIG_SPIRAM_SPEED=200',
                 '# CONFIG_SPIRAM_XIP_FROM_PSRAM is not set',
                 'CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=2', 'CONFIG_ESP_LVGL_ADAPTER_ROTATE_IN_DRAW=y',
                 'CONFIG_ESPTOOLPY_FLASHSIZE="' + ('16MB' if board == 'tab5' else '32MB') + '"'):
    if expected not in config.splitlines():
        raise ValueError('Unexpected SDK configuration: ' + expected)
description = json.loads((args.build / 'project_description.json').read_text())
ppa = Path(description['build_component_info']['esp_driver_ppa']['dir']).resolve()
if ppa == (app / 'components/esp_driver_ppa').resolve():
    raise ValueError('V5 must use the stock ESP-IDF PPA driver')
lv_init = (app / 'managed_components/lvgl__lvgl/src/lv_init.c').read_text()
if 'esp_lvgl_adapter: release the global OS mutex' not in lv_init:
    raise ValueError('The OS-mutex cleanup patch is missing')
backend = (app / 'managed_components/lvgl__lvgl/src/osal/lv_freertos.c').read_text()
if 'esp_lvgl_adapter: stop remote execution before deleting' not in backend:
    raise ValueError('Thread deletion barrier is missing')
compile_commands = json.loads((args.build / 'compile_commands.json').read_text())
commands = [entry['command'] for entry in compile_commands if entry['file'].endswith('/src/lv_init.c')]
if len(commands) != 1 or '-DESP_LVGL_ADAPTER_OS_MUTEX_CLEANUP=1' not in commands[0]:
    raise ValueError('The OS-mutex cleanup was not enabled in the real LVGL compilation')
merged = image.read_bytes()
for offset, filename in flasher['flash_files'].items():
    segment = (args.build / filename).read_bytes()
    start = int(offset, 0)
    if merged[start:start + len(segment)] != segment:
        raise ValueError('Flash segment mismatch: ' + filename)
shutil.copy2(app / 'sdkconfig', args.out / f'{board}-sdkconfig.txt')
shutil.copy2(app / 'dependencies.lock', args.out / f'{board}-dependencies.lock')
metadata.update(esp_idf='5.5.6', lvgl='9.6.0', flash_offset='0x0',
                firmware_size=len(merged), firmware_sha256=hashlib.sha256(merged).hexdigest(),
                flash_settings=flasher['flash_settings'])
(args.out / f'{board}-manifest.json').write_text(json.dumps(metadata, indent=2) + '\n')
print(json.dumps(metadata, indent=2))
