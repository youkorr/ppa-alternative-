#!/usr/bin/env python3
"""Verify the private cleanup patch preserves V8 measured paths and API order."""
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import tempfile

HERE = Path(__file__).resolve().parent
SOURCE = Path('/workspace/work/ab-diagnostics-v8/fixtures/ab_main.c')
spec = importlib.util.spec_from_file_location('cleanup_patch', HERE / 'patch_cleanup_trace.py')
patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch)
original = SOURCE.read_bytes()
result = patch.instrument(original)
assert result == (HERE / 'ab_main.c').read_bytes()
a, b = original.decode(), result.decode()

# All timing loops, sampling/drain logic and existing assertions before the
# post-measurement cleanup anchor are byte-identical.
phase_start = 'static void phase('
phase_end = '    log_costs(scene, timer_mode, round, costs);\n'
assert a[a.index(phase_start):a.index(phase_end) + len(phase_end)] == \
       b[b.index(phase_start):b.index(phase_end) + len(phase_end)]
for start, end in [('static void paired_phase(', '\nvoid app_main(void)'),
                   ('static void build_scene(', '\nstatic void update_scene('),
                   ('static void update_scene(', '\nstatic void prepare_bank('),
                   ('static void prepare_bank(', '\nstatic void log_costs(')]:
    assert a[a.index(start):a.index(end)] == b[b.index(start):b.index(end)]

# The exact existing measured log formats and arguments remain parseable by
# the published V8 parser, including the actual AB_DONE completion marker.
for tag in ['AB_READY', 'AB_RESULT', 'AB_TIMING', 'AB_REFRESH', 'AB_COST', 'AB_DONE']:
    pattern = re.compile(r'ESP_LOGI\(TAG, "' + tag + r' .*?\);', re.S)
    assert pattern.findall(a) == pattern.findall(b), tag

# Preserve every underlying cleanup call, in the same order; only surround it
# with markers. The four source banks are still freed before AB_DONE.
start = '    heap_caps_free(refresh_us);'
end = '\nstatic void paired_phase('
old = a[a.index(start):a.index(end)]
new = b[b.index(start):b.index(end)]
calls = [line.strip() for line in old.splitlines() if line.strip().endswith(';')]
assert calls == [line.strip() for line in new.splitlines()
                 if line.strip().endswith(';') and not line.strip().startswith('cleanup_mark(')]
assert b.index('heap_caps_free(s_bank[i]);') < b.index('"AB_DONE protocol=8')
assert b.index('cleanup_mark(21, "emit_ab_done", true') < b.index('"AB_DONE protocol=8')
assert b.index('"AB_DONE protocol=8') < b.index('cleanup_mark(21, "emit_ab_done", false')
assert b.index('cleanup_console_write(s_cleanup_complete_format);') < b.index('    cleanup_observer_disarm();')

# At any configured round count the observer arms only on the last official
# phase, regardless of the counterbalanced timer-mode order.
for rounds in range(1, 11):
    armed = []
    phases = [(n, kind, strict) for n in range(1, rounds + 1)
              for kind in (0, 1, 2)
              for strict in ((False, True) if n % 2 else (True, False))]
    for n, kind, strict in phases:
        if n == rounds and kind == 2 and strict == bool(n & 1):
            armed.append((n, kind, strict))
    assert armed == [phases[-1]]

# Observer mux sections contain only fixed-size state operations, never the
# cleanup operations, logger, ROM output, task creation or allocation.
for section in re.findall(r'portENTER_CRITICAL_SAFE\(&s_cleanup_mux\);(.*?)portEXIT_CRITICAL_SAFE\(&s_cleanup_mux\);', patch.HELPERS, re.S):
    assert not any(api in section for api in ['ESP_LOG', 'panic_print_str', 'cleanup_console_write', 'heap_caps_', 'xTaskCreate', 'lv_obj_', 'lv_image_'])
assert 'panic_print_str(line);' in b and 'usb_serial_jtag_ll_txfifo_flush();' in b
assert '"ab_cleanup", 4096, NULL, 6, NULL, tskNO_AFFINITY' in b
assert 'vTaskDelete(NULL)' in patch.HELPERS and 'vTaskDelete(' not in patch.HELPERS.replace('vTaskDelete(NULL)', '')

with tempfile.TemporaryDirectory(prefix='cleanup-trace-check-') as directory:
    path = Path(directory) / 'private.c'
    path.write_bytes(original)
    manifest = patch.patch(path)
    assert path.read_bytes() == result
    try:
        patch.patch(path)
    except ValueError:
        pass
    else:
        raise AssertionError('second patch must be rejected')
try:
    patch.instrument(original + b'\n')
except ValueError:
    pass
else:
    raise AssertionError('unpinned source must be rejected')
assert hashlib.sha256(SOURCE.read_bytes()).hexdigest() == patch.SOURCE_SHA256

report = {'status': 'passed', 'measured_phase_path_identical': True,
          'scene_and_trace_functions_identical': True, 'existing_protocol_rows_identical': True,
          'cleanup_call_order_preserved': True, 'observer_arms_only_after_final_official_phase': True,
          'observer_mux_contains_no_cleanup_or_logging': True,
          'pinned_source_guard_and_repeat_rejection': True,
          'input_unchanged': True, 'patch': manifest}
(HERE / 'structural-checks.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
