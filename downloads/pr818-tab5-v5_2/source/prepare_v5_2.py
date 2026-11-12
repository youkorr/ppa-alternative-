# SPDX-FileCopyrightText: 2026 youkorr
# SPDX-License-Identifier: MIT
"""Measure the Tab5 heap after bounded idle-task reclamation, before Unity's check."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

import prepare_v5


def make(args):
    app = prepare_v5.make(args)
    main = app / 'main/test_esp_lvgl_adapter_features.c'
    prepare_v5.replace_once(main, 'void setUp(void)\n{',
                            '#include "task_settle.h"\n\nvoid setUp(void)\n{\n'
                            '    s_pr818_tasks_before_test = uxTaskGetNumberOfTasks();')
    prepare_v5.replace_once(main,
                            '    test_deinit_display();\n    size_t after_free_8bit =',
                            '    test_deinit_display();\n'
                            '    const size_t before_idle = heap_caps_get_free_size(MALLOC_CAP_8BIT);\n'
                            '    const bool ready = pr818_wait_deleted_tasks(s_pr818_tasks_before_test);\n'
                            '    size_t after_free_8bit =')
    prepare_v5.replace_once(main,
                            '    unity_utils_check_leak(s_before_free_8bit,',
                            '    ESP_LOGI(TAG, "PR818_TASK_SETTLE wait_ms=%u tasks_expected=%u tasks_now=%u reclaimed=%ld ready=%u",\n'
                            '             (unsigned)s_pr818_settle_ms, (unsigned)s_pr818_tasks_before_test,\n'
                            '             (unsigned)s_pr818_tasks_after_settle,\n'
                            '             (long)((int32_t)after_free_8bit - (int32_t)before_idle), (unsigned)ready);\n'
                            '    TEST_ASSERT_TRUE_MESSAGE(ready, "Deleted tasks did not settle before the heap check");\n'
                            '    unity_utils_check_leak(s_before_free_8bit,')
    here = Path(__file__).resolve().parent
    shutil.copyfile(here / 'task_settle.h', app / 'main/task_settle.h')
    prepare_v5.replace_once(app / 'main/board_bench_config.h', '"after_v5"', '"after_v5_2"')
    path = args.out / 'benchmark.json'
    metadata = json.loads(path.read_text())
    metadata.update(revision='after_v5_2', heap_measurement_after_task_settle=True,
                    task_settle_timeout_ms=200, task_settle_poll_ms=10, task_settle_stable_samples=2, thread_shutdown_barrier=True,
                    task_settle_sha256=hashlib.sha256((here / 'task_settle.h').read_bytes()).hexdigest())
    path.write_text(json.dumps(metadata, indent=2) + '\n')
    return app


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--fixtures', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--idf', type=Path, required=True)
    parser.add_argument('--board', choices=['tab5'], default='tab5')
    print(make(parser.parse_args()))
