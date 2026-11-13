#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 youkorr
# SPDX-License-Identifier: MIT
"""Protocol tests use synthetic records only; they are not hardware results."""
import copy
import importlib.util
from pathlib import Path
import re
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location('v8_parser', HERE / 'parse_logs.py')
M = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(M)


def generated(variant='native64', board='tab5'):
    native = variant == 'native64'
    h = dict(protocol='9', board=board, variant=variant,
             source_commit=('7' if native else 'd') * 40, source_tree=('6' if native else '3') * 40,
             logical='1280x720' if board == 'tab5' else '1024x600', rotation='270' if board == 'tab5' else '180',
             panel_buffers='3', cpu_mhz='360', psram_mhz='200', l2_kib='128', batch='64' if native else '0',
             rounds='2', warmup='16', frames='64', dpi_probe='1', dashboard_full='0', sensor_capture='0',
             draw_units='2', ppa_draw_requested='0', sync_modes='eager,deferred', timer_policy='automatic', paired_order='alternating',
             cost_probe='1', refresh_events='1', timings='lock,update,refresh,unlock,yield')
    result = [('AB_READY', h)]
    for n in (1, 2):
        for scene in ('dashboard', 'changing_rgb565', 'fixed_flash_off') + (('fixed_flash_warm_cache',) if native else ()):
            for mode in (('eager', 'deferred') if n == 1 else ('deferred', 'eager')):
                fps = (12 if native else 10) + (1 if mode == 'deferred' else 0)
                ident = dict(protocol='9', board=board, variant=variant, sync_mode=mode, scene=scene, round=str(n))
                produced = int(64e6 / fps)
                warm = scene == 'fixed_flash_warm_cache'
                row = dict(ident, defer_empty_sync=str(int(mode == 'deferred')), batch=h['batch'], frames='64', produced_us=str(produced), submitted_fps=str(fps),
                           refresh_mean_us='10000', refresh_median_us='10000', refresh_p95_us='11000', refresh_worst_us='12000',
                           scanout_metric='GENERATION_SELECTED_FOR_DMA', selected='64', unselected='0', scanout_cohort_us=str(produced), scanout_fps=str(fps),
                           draw_calls='64', scanout_irqs='128', invalid_indices='0', failures='0',
                           cache_budget='4194304' if warm else '0', cache_hits='128' if warm else '0', cache_bytes='1844864' if warm else '0',
                           cores='2' if native else '0', turner_failed='0', native_stats=str(int(native)), tile_internal=str(int(native)), tiles_in_turn='4' if native else '0',
                           batches='64' if native else '0', waits='192' if native else '0', bytes_turned='100000' if native else '0',
                           free_before='20000000', internal_before='100000', free_after='20000000', internal_after='100000', minimum_free='19990000', minimum_internal='90000',
                           largest_internal_before='30000', largest_internal_after='30000', heap_ok='1', idle_yields='4', tick_hz='1000')
                timing = dict(ident, frames='64', handoff_us='10', lock_us='12800', update_us='32000', refresh_us='640000', timer_policy_us='640',
                              unlock_us='3200', yield_us='4000', residual_us=str(produced - 692650), produced_us=str(produced), drain_us='1000', close_lock_us='50',
                              lock_calls='64', update_calls='64', refresh_calls='64', unlock_calls='64', yield_calls='4')
                events = dict(ident, window='trace_plus_drain_and_close_lock', manual_starts='64', manual_ready='64', automatic_starts='4', automatic_ready='4',
                              manual_us='639000', automatic_us='4000', requests='128', requests_running='128', strict_pauses='0', nested='0', unmatched_ready='0', inclusive='1')
                cost = dict(ident, stage='refresh', kind='cache', calls='64', us='1000', bytes='128000', pixels='0', errors='0', pending='0', flags_or='5', c2m_bytes='128000', m2c_bytes='0', inclusive='1')
                result.extend([('AB_RESULT', row), ('AB_TIMING', timing), ('AB_REFRESH', events), ('AB_COST', cost)])
    done = dict(protocol='9', board=board, variant=variant, rounds='2', sync_modes='eager,deferred', timer_policy='automatic', hardware_measurement='COMPLETE', compare_only_matching_board_scene_and_sync_mode='1')
    result.append(('AB_DONE', done))
    return result


def render(rows):
    return '\n'.join('[00:00:01]I (1000) ab_bench: ' + tag + ' ' + ' '.join(f'{k}={v}' for k, v in row.items()) for tag, row in rows) + '\n'


class ParserTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.index = 0

    def parse(self, rows):
        self.index += 1
        p = Path(self.temp.name) / f'capture{self.index}.txt'
        p.write_text(render(rows))
        return M.parse(p)

    def altered(self, tag, **fields):
        rows = generated('native64')
        row = next(r for t, r in rows if t == tag and r.get('sync_mode') == 'deferred')
        row.update({k: str(v) for k, v in fields.items()})
        return rows

    def test_complete_paired_captures_and_same_mode_comparison(self):
        report = M.summarize([self.parse(generated())])
        self.assertEqual(len(report['results']), 8)
        deferred = next(x for x in report['results'] if x['sync_mode'] == 'deferred' and x['scene'] == 'changing_rgb565')
        self.assertAlmostEqual(deferred['deferred_vs_eager_percent'], (13 / 12 - 1) * 100)
        self.assertTrue(all(x['gain_vs_official_same_sync_mode_percent'] is None for x in report['results']))

    def test_two_boards_kept_separate(self):
        report = M.summarize([self.parse(generated()), self.parse(generated('native64', 'waveshare-7b'))])
        self.assertEqual({x['board'] for x in report['results']}, {'tab5', 'waveshare-7b'})

    def test_missing_completion_rejected(self):
        with self.assertRaisesRegex(ValueError, 'complete AB_READY'):
            self.parse(generated()[:-1])

    def test_incomplete_mode_or_duplicate_row_rejected(self):
        for rows in (generated()[:2] + generated()[3:], generated()[:-1] + [generated()[1], generated()[-1]]):
            with self.assertRaisesRegex(ValueError, 'missing or duplicate'):
                self.parse(rows)

    def test_timer_override_rejected(self):
        with self.assertRaisesRegex(ValueError, 'timer was overridden'):
            self.parse(self.altered('AB_REFRESH', strict_pauses=1))

    def test_wrong_engine_policy_rejected(self):
        with self.assertRaisesRegex(ValueError, 'synchronization policy'):
            self.parse(self.altered('AB_RESULT', defer_empty_sync=0))

    def test_manual_count_rejected(self):
        with self.assertRaisesRegex(ValueError, 'manual refresh-event'):
            self.parse(self.altered('AB_REFRESH', manual_starts=63))

    def test_timing_sum_and_call_counts_rejected(self):
        for fields in ({'lock_us': 12801}, {'lock_calls': 63}):
            with self.assertRaises(ValueError):
                self.parse(self.altered('AB_TIMING', **fields))

    def test_inconsistent_fps_or_mean_rejected(self):
        for fields in ({'scanout_fps': 999}, {'submitted_fps': 999}, {'refresh_mean_us': 1}, {'refresh_p95_us': 1}):
            with self.assertRaises(ValueError):
                self.parse(self.altered('AB_RESULT', **fields))

    def test_invalid_scanout_or_heap_rejected(self):
        for fields in ({'scanout_metric': 'UNAVAILABLE'}, {'heap_ok': 0}, {'unselected': 1}, {'failures': 1}):
            with self.assertRaises(ValueError):
                self.parse(self.altered('AB_RESULT', **fields))

    def test_failed_or_impossible_cost_rejected(self):
        for fields in ({'errors': 1}, {'pending': 65}, {'pending': 1}, {'stage': 'unknown'}, {'inclusive': 0}):
            with self.assertRaises(ValueError):
                self.parse(self.altered('AB_COST', **fields))

    def test_duplicate_cost_cell_rejected(self):
        rows = generated()
        rows.insert(-1, copy.deepcopy(next(x for x in rows if x[0] == 'AB_COST')))
        with self.assertRaisesRegex(ValueError, 'duplicate phase/stage/category'):
            self.parse(rows)

    def test_mixed_identity_and_configuration_rejected(self):
        with self.assertRaisesRegex(ValueError, 'mixed identity'):
            self.parse(self.altered('AB_RESULT', board='waveshare-7b'))
        second = generated('native64')
        second[0][1]['cpu_mhz'] = '400'
        with self.assertRaisesRegex(ValueError, 'configuration mismatch'):
            M.summarize([self.parse(generated()), self.parse(second)])

    def test_tick_mismatch_rejected(self):
        with self.assertRaisesRegex(ValueError, 'inconsistent RTOS tick'):
            self.parse(self.altered('AB_RESULT', tick_hz=100))

    def test_cache_contamination_rejected(self):
        rows = generated('native64')
        next(row for tag, row in rows if tag == 'AB_RESULT' and row.get('scene') == 'changing_rgb565')['cache_hits'] = '1'
        with self.assertRaisesRegex(ValueError, 'changing frames'):
            self.parse(rows)

    def test_duplicate_capture_rejected(self):
        capture = self.parse(generated())
        with self.assertRaisesRegex(ValueError, 'duplicate complete capture'):
            M.summarize([capture, capture])

    def test_crash_after_done_rejected(self):
        rows = generated()
        p = Path(self.temp.name) / 'crash.txt'
        p.write_text(render(rows) + 'Guru Meditation Error: CPU panic\n')
        with self.assertRaisesRegex(ValueError, 'crash'):
            M.parse(p)

    def test_declared_pair_order_enforced(self):
        rows = generated()
        rows[1:9] = rows[5:9] + rows[1:5]
        with self.assertRaisesRegex(ValueError, 'paired mode/scene order'):
            self.parse(rows)

    def test_log_fields_match_real_firmware_source(self):
        source_path = HERE / 'ab_main.c'
        if not source_path.exists(): source_path = HERE.parent / 'main-work/ab_main.c'
        source = source_path.read_text()
        generated_rows = generated('native64')
        for tag in ('AB_READY', 'AB_RESULT', 'AB_TIMING', 'AB_REFRESH', 'AB_COST', 'AB_DONE'):
            fragment = re.search(r'ESP_LOGI\(TAG, "' + tag + r' (.*?)\n\s*AB_BOARD_NAME', source, re.S)
            self.assertIsNotNone(fragment, tag)
            keys = set(re.findall(r'(\w+)=', fragment[1]))
            row = next(row for name, row in generated_rows if name == tag)
            self.assertEqual(keys, set(row), tag)


if __name__ == '__main__':
    unittest.main(verbosity=2)
