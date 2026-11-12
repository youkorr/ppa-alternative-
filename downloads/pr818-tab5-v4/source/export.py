# SPDX-FileCopyrightText: 2026 youkorr
# SPDX-License-Identifier: MIT
"""Export the real flash segments and configuration from an IDF build."""
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
flasher = json.loads((args.build / 'flasher_args.json').read_text())
args.out.mkdir(parents=True, exist_ok=True)
image = args.out / 'tab5-after_v4.bin'
command = [sys.executable, '-m', 'esptool', '--chip', 'esp32p4', 'merge_bin',
           '-o', str(image), '--flash_mode', flasher['flash_settings']['flash_mode'],
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
                 'CONFIG_ESPTOOLPY_FLASHSIZE="16MB"'):
    if expected not in config.splitlines():
        raise ValueError('Unexpected SDK configuration: ' + expected)
description = json.loads((args.build / 'project_description.json').read_text())
ppa_path = Path(description['build_component_info']['esp_driver_ppa']['dir']).resolve()
expected_path = (app / 'components/esp_driver_ppa').resolve()
if metadata['ppa_workaround'] == 'espressif_bypass' and ppa_path != expected_path:
    raise ValueError(f'Build did not use the local PPA workaround: {ppa_path}')
if metadata['ppa_workaround'] == 'stock' and ppa_path == expected_path:
    raise ValueError('Stock-driver test must not use the local bypass override')
shutil.copy2(app / 'sdkconfig', args.out / 'sdkconfig.txt')
shutil.copy2(app / 'dependencies.lock', args.out / 'dependencies.lock')
if metadata['ppa_workaround'] == 'espressif_bypass':
    (args.out / 'source').mkdir(parents=True, exist_ok=True)
    shutil.copy2(args.stage / 'esp-idf-5.5.6-ppa-bypass.patch', args.out / 'source/esp-idf-5.5.6-ppa-bypass.patch')
metadata.update(esp_idf='5.5.6', lvgl='9.6.0', flash_offset='0x0',
                firmware_size=image.stat().st_size,
                firmware_sha256=hashlib.sha256(image.read_bytes()).hexdigest(),
                flash_settings=flasher['flash_settings'])
(args.out / 'manifest.json').write_text(json.dumps(metadata, indent=2) + '\n')
print(json.dumps(metadata, indent=2))
