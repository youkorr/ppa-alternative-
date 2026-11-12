# SPDX-FileCopyrightText: 2026 youkorr
# SPDX-License-Identifier: MIT
"""Prepare the short V5 restart test for Tab5 or Waveshare 7B."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
from types import SimpleNamespace

import prepare


def replace_once(path, before, after):
    text = path.read_text()
    if text.count(before) != 1:
        raise ValueError(f'Expected one occurrence in {path}: {before!r}')
    path.write_text(text.replace(before, after, 1))


def make(args):
    if (args.idf / 'version.txt').read_text().strip() != 'v5.5.6':
        raise ValueError('Use ESP-IDF v5.5.6 for this comparison')
    prepare.prepare(SimpleNamespace(repo=args.repo, fixtures=args.fixtures, out=args.out,
                                   board=args.board, revision='after_v2'))
    # git archive replaces tracked files but does not remove files introduced
    # by this candidate in an earlier card's generated tree.
    (args.out / prepare.ADAPTER / 'tools/patch_lvgl_os_cleanup.py').unlink(missing_ok=True)
    app = args.out.resolve() / prepare.ADAPTER / 'test_apps/lvgl9'
    # The original SDK driver is used, exactly as in V4.
    override = app / 'components/esp_driver_ppa'
    if override.exists():
        shutil.rmtree(override)
    patch = Path(__file__).resolve().parent / 'candidate-v5.patch'
    subprocess.run(['git', 'apply', '--check', str(patch)], cwd=args.out, check=True)
    subprocess.run(['git', 'apply', str(patch)], cwd=args.out, check=True)
    main = app / 'main/test_esp_lvgl_adapter_features.c'
    text = main.read_text()
    at = text.rindex('void app_main(void)')
    main.write_text(text[:at] + '#include "memory_bench.inc.c"\n')
    shutil.copyfile(Path(__file__).resolve().parent / 'memory_bench.inc.c', app / 'main/memory_bench.inc.c')
    replace_once(app / 'main/board_bench_config.h', '"after_v2"', '"after_v5"')
    with (app / 'main/CMakeLists.txt').open('a') as out:
        out.write('\n# Observe actual LVGL OS mutex ownership without changing the renderer.\n'
                  'target_link_options(${COMPONENT_LIB} INTERFACE\n'
                  '    "-Wl,--wrap=lv_mutex_init" "-Wl,--wrap=lv_mutex_delete")\n')
    if args.board == 'tab5':
        lcd = app / 'components/hw_init/lcd/lcd_init_mipi_dsi.c'
        replace_once(lcd, '#include "esp_check.h"', '#include "esp_check.h"\n#include "esp_intr_alloc.h"\n#include <stdio.h>')
        replace_once(lcd, '    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9881c(s_mipi_dbi_io, &panel_config, &s_panel_handle));',
                     '    esp_err_t ret = esp_lcd_new_panel_ili9881c(s_mipi_dbi_io, &panel_config, &s_panel_handle);\n'
                     '    if (ret != ESP_OK) {\n'
                     '        ESP_LOGE(TAG, "PR818_PANEL_ALLOC_FAILED: %s", esp_err_to_name(ret));\n'
                     '        esp_intr_dump(stdout);\n'
                     '        (void)hw_lcd_deinit();\n'
                     '        return ret;\n'
                     '    }')
    path = args.out / 'benchmark.json'
    metadata = json.loads(path.read_text())
    metadata.update(revision='after_v5', rendering_patch='source/candidate-v5.patch',
                    rendering_patch_sha256=hashlib.sha256(patch.read_bytes()).hexdigest(),
                    ppa_workaround='stock', cache_prepare_max_bytes=131072,
                    ppa_no_progress_timeout_ms=250, memory_cycles=64, memory_warmups=4,
                    memory_cumulative_budget_bytes=256, os_mutex_cleanup=True,
                    test_mode='restart_memory_only', hardware_validation='pending')
    path.write_text(json.dumps(metadata, indent=2) + '\n')
    return app


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--fixtures', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--idf', type=Path, required=True)
    parser.add_argument('--board', choices=['tab5', 'waveshare-7b'], required=True)
    print(make(parser.parse_args()))
