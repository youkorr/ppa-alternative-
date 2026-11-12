# SPDX-FileCopyrightText: 2026 youkorr
# SPDX-License-Identifier: MIT
"""Prepare the Tab5 follow-up: stop on failure, retain PPA diagnostics.

The optional bypass is the workaround shipped by esp_lvgl_adapter, adapted
to IDF 5.5.6. It overrides a component in this test app; the SDK is untouched.
"""
import argparse
import difflib
import hashlib
import json
from pathlib import Path
import shutil
from types import SimpleNamespace

import prepare


def replace_once(path, before, after):
    text = path.read_text()
    if text.count(before) != 1:
        raise ValueError(f"Expected one occurrence in {path}: {before!r}")
    path.write_text(text.replace(before, after, 1))


def make(args):
    if not (args.idf / 'version.txt').read_text().strip().startswith('v5.5.6'):
        raise ValueError('This reproduction requires ESP-IDF v5.5.6')
    prepare.prepare(SimpleNamespace(repo=args.repo, fixtures=args.fixtures,
                                   out=args.out, board='tab5', revision='after_v2'))
    app = args.out.resolve() / prepare.ADAPTER / 'test_apps/lvgl9'
    replace_once(app / 'main/CMakeLists.txt', 'REQUIRES hw_init unity esp_timer',
                 'REQUIRES hw_init unity esp_timer esp_driver_ppa')
    main = app / 'main/test_esp_lvgl_adapter_features.c'
    replace_once(app / 'main/board_bench_config.h', '"after_v2"', '"after_v3"')
    replace_once(main, '    unity_run_test_by_name("PR818 board comparison");\n'
                 '    for (int i = 0; i < 16; i++)\n'
                 '        unity_run_test_by_name("adapter render threads can be stopped repeatedly");\n'
                 '    ESP_LOGI(TAG, "PR818_RESTART_OK count=16");',
                 '    const unsigned failures = Unity.TestFailures;\n'
                 '    unity_run_test_by_name("PR818 board comparison");\n'
                 '    if (Unity.TestFailures != failures) {\n'
                 '        ESP_LOGE(TAG, "PR818_ABORT: benchmark failed; no further display restart is attempted");\n'
                 '        unity_run_menu();\n'
                 '        return;\n'
                 '    }\n'
                 '    for (int i = 0; i < 16; i++) {\n'
                 '        unity_run_test_by_name("adapter render threads can be stopped repeatedly");\n'
                 '        if (Unity.TestFailures != failures) {\n'
                 '            ESP_LOGE(TAG, "PR818_ABORT: lifecycle failed at cycle %d", i + 1);\n'
                 '            unity_run_menu();\n'
                 '            return;\n'
                 '        }\n'
                 '    }\n'
                 '    ESP_LOGI(TAG, "PR818_RESTART_OK count=16");')
    # Preserve an interrupt allocation error as a Unity failure with its table,
    # rather than calling abort() before the test runner can report the result.
    lcd = app / 'components/hw_init/lcd/lcd_init_mipi_dsi.c'
    replace_once(lcd, '#include "esp_check.h"',
                 '#include "esp_check.h"\n#include "esp_intr_alloc.h"\n#include <stdio.h>')
    before = '    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9881c(s_mipi_dbi_io, &panel_config, &s_panel_handle));'
    after = ('    esp_err_t ret = esp_lcd_new_panel_ili9881c(s_mipi_dbi_io, &panel_config, &s_panel_handle);\n'
             '    if (ret != ESP_OK) {\n'
             '        ESP_LOGE(TAG, "PR818_PANEL_ALLOC_FAILED: %s", esp_err_to_name(ret));\n'
             '        esp_intr_dump(stdout);\n'
             '        const esp_err_t cleanup = hw_lcd_deinit();\n'
             '        if (cleanup != ESP_OK)\n'
             '            ESP_LOGE(TAG, "Partial panel cleanup: %s", esp_err_to_name(cleanup));\n'
             '        return ret;\n'
             '    }')
    replace_once(lcd, before, after)
    # Make PPA stalls visible even when LV_USE_LOG is disabled in both kits.
    bench = app / 'main/board_bench.inc.c'
    replace_once(bench, '#include "board_bench_config.h"',
                 '#include "board_bench_config.h"\n#include "../../../src/display/rotate_in_draw/lv_draw_ortho_ppa.h"')
    replace_once(bench, 'static board_metrics_t s_metrics;',
                 'static board_metrics_t s_metrics;\n\n'
                 'static void board_stall_line(const char *line, void *user)\n'
                 '{\n    (void)user;\n    ESP_LOGE(TAG, "PR818_PPA_STALL: %s", line);\n}\n')
    replace_once(bench, '    const int64_t warm_us = esp_timer_get_time() - warm_started;',
                 '    const int64_t warm_us = esp_timer_get_time() - warm_started;\n'
                 '    if (lv_draw_ortho_get_stats()->turner_failed) {\n'
                 '        ESP_LOGE(TAG, "PR818_PPA_PHASE: %s round %d, failed during or before warm-up", scene, round);\n'
                 '        lv_draw_ortho_ppa_describe_stall(board_stall_line, NULL);\n'
                 '    }')
    replace_once(bench, '    TEST_ASSERT_FALSE_MESSAGE(stats.turner_failed,',
                 '    if (stats.turner_failed) {\n'
                 '        ESP_LOGE(TAG, "PR818_PPA_PHASE: %s round %d", scene, round);\n'
                 '        lv_draw_ortho_ppa_describe_stall(board_stall_line, NULL);\n'
                 '    }\n'
                 '    TEST_ASSERT_FALSE_MESSAGE(stats.turner_failed,')
    replace_once(bench, '    ESP_LOGI(TAG, "PR818_START board=',
                 '    ESP_LOGI(TAG, "PR818_PPA_WORKAROUND mode=%s", BOARD_PPA_WORKAROUND);\n'
                 '    ESP_LOGI(TAG, "PR818_START board=')
    config = app / 'main/board_bench_config.h'
    config.write_text(config.read_text() + f'#define BOARD_PPA_WORKAROUND "{args.ppa_workaround}"\n')
    metadata = json.loads((args.out / 'benchmark.json').read_text())
    metadata.update(revision='after_v3', ppa_workaround=args.ppa_workaround,
                    hardware_validation='pending', stops_after_unity_failure=True,
                    stall_log_independent_of_lvgl_logging=True)
    target = app / 'components/esp_driver_ppa'
    if target.exists():
        # This is the generated application's local override, never the SDK.
        shutil.rmtree(target)
    if args.ppa_workaround == 'espressif_bypass':
        source = args.idf / 'components/esp_driver_ppa'
        shutil.copytree(source, target, dirs_exist_ok=True)
        ppa = target / 'src/ppa_srm.c'
        original = ppa.read_text()
        begin = original.index('    // Hardware bug workaround (DIG-734)')
        end = original.index('    ppa_ll_srm_bypass_mb_order(platform->hal.dev, bypass_mb_order);', begin)
        end += len('    ppa_ll_srm_bypass_mb_order(platform->hal.dev, bypass_mb_order);')
        changed = (original[:begin] + '    // esp_lvgl_adapter temporary PPA hang workaround (DIG-734).\n'
                   '    ppa_ll_srm_bypass_mb_order(platform->hal.dev, true);' + original[end:])
        ppa.write_text(changed)
        (args.out / 'esp-idf-5.5.6-ppa-bypass.patch').write_text(''.join(difflib.unified_diff(
            original.splitlines(keepends=True), changed.splitlines(keepends=True),
            fromfile='a/components/esp_driver_ppa/src/ppa_srm.c',
            tofile='b/components/esp_driver_ppa/src/ppa_srm.c')))
        metadata['ppa_original_sha256'] = hashlib.sha256(original.encode()).hexdigest()
        metadata['ppa_override_sha256'] = hashlib.sha256(changed.encode()).hexdigest()
        metadata['workaround_reference'] = ('components/display/tools/esp_lvgl_adapter/'
                                           '0001-bugfix-ppa-Temporary-fix-for-the-PPA-hang-issue.patch')
    else:
        (args.out / 'esp-idf-5.5.6-ppa-bypass.patch').unlink(missing_ok=True)
    (args.out / 'benchmark.json').write_text(json.dumps(metadata, indent=2) + '\n')
    return app


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--fixtures', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--idf', type=Path, required=True)
    parser.add_argument('--ppa-workaround', choices=['stock', 'espressif_bypass'], default='espressif_bypass')
    print(make(parser.parse_args()))
