#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 youkorr
# SPDX-License-Identifier: MIT
"""Validate complete V9 paired-timer serial captures and summarize matching tests.

Diagnostic category times are inclusive, may overlap, and must never be added.
This parser neither measures optical panel FPS nor estimates physical bus traffic.
"""
import argparse
from collections import defaultdict
import json
import math
from pathlib import Path
import re
import statistics

FIELDS = re.compile(r'(\w+)=([^\s]+)')
ROW = re.compile(r'\b(AB_READY|AB_RESULT|AB_TIMING|AB_REFRESH|AB_COST|AB_DONE) (.*)')
CRASH = re.compile(r'Guru Meditation Error|assert failed:|abort\(\) was called|CORRUPT HEAP:|Task watchdog got triggered')
MODES = ('eager', 'deferred')
STAGES = ('setup', 'lock', 'update', 'refresh', 'unlock', 'yield', 'drain', 'background')
KINDS = ('copy', 'cache', 'srm', 'fill', 'blend', 'native_wait')
COMMON = ('protocol', 'logical', 'rotation', 'panel_buffers', 'cpu_mhz', 'psram_mhz',
          'l2_kib', 'frames', 'rounds', 'warmup', 'dpi_probe', 'dashboard_full',
          'sensor_capture', 'draw_units', 'ppa_draw_requested', 'sync_modes', 'timer_policy',
          'paired_order', 'cost_probe', 'refresh_events')


def integer(row, name, minimum=0):
    try:
        value = int(row[name])
    except (KeyError, ValueError) as exc:
        raise ValueError(f'missing or invalid integer {name}') from exc
    if value < minimum:
        raise ValueError(f'{name} must be >= {minimum}')
    return value


def number(row, name, positive=False):
    try:
        value = float(row[name])
    except (KeyError, ValueError) as exc:
        raise ValueError(f'missing or invalid number {name}') from exc
    if not math.isfinite(value) or value < 0 or (positive and value == 0):
        raise ValueError(f'{name} must be finite and {"positive" if positive else "nonnegative"}')
    return value


def phase_key(row):
    return row['sync_mode'], row['scene'], integer(row, 'round', 1)


def _fields(text):
    pairs = FIELDS.findall(text)
    if len(dict(pairs)) != len(pairs):
        raise ValueError('duplicate field in one protocol row')
    return dict(pairs)


def parse(path):
    path = Path(path)
    records = defaultdict(list)
    ordered_tags = []
    for line in path.read_text(errors='replace').splitlines():
        if CRASH.search(line):
            raise ValueError(f'{path}: crash or heap corruption in capture')
        match = ROW.search(line)
        if match:
            ordered_tags.append(match[1])
            records[match[1]].append(_fields(match[2]))
    try:
        if len(records['AB_READY']) != 1 or len(records['AB_DONE']) != 1:
            raise ValueError('one complete AB_READY … AB_DONE capture required; split multiple boots')
        if ordered_tags[0] != 'AB_READY' or ordered_tags[-1] != 'AB_DONE':
            raise ValueError('protocol rows outside the AB_READY … AB_DONE window')
        header, done = records['AB_READY'][0], records['AB_DONE'][0]
        for key in COMMON:
            if key not in header:
                raise ValueError(f'missing header field {key}')
        if header['protocol'] != '9' or done['protocol'] != '9':
            raise ValueError('only protocol9 captures can be compared with this parser')
        if header['variant'] not in ('native64',):
            raise ValueError('V9 capture must identify official or native64')
        if header['board'] not in ('tab5', 'waveshare-7b'):
            raise ValueError('unknown board')
        if header['sync_modes'] != 'eager,deferred' or header['paired_order'] != 'alternating':
            raise ValueError('both paired eager/deferred synchronization modes with alternating order required')
        if any(header[k] != '1' for k in ('dpi_probe', 'cost_probe', 'refresh_events')):
            raise ValueError('scanout, cost and refresh-event probes must all be enabled')
        if header['timer_policy'] != 'automatic':
            raise ValueError('normal automatic timer policy required')
        if header['sensor_capture'] != '0' or header['draw_units'] != '2' or header['ppa_draw_requested'] != '0':
            raise ValueError('unexpected sensor/draw-worker/PPA profile for V9 kit')
        if integer(header, 'panel_buffers', 1) != 3:
            raise ValueError('V9 requires three physical panel buffers')
        native = header['variant'] == 'native64'
        if integer(header, 'batch') != (64 if native else 0):
            raise ValueError('variant batch limit does not match header')
        frames, rounds = integer(header, 'frames', 16), integer(header, 'rounds', 1)
        integer(header, 'warmup', 1)
        for key in ('source_commit', 'source_tree'):
            if not re.fullmatch(r'[0-9a-f]{40}', header.get(key, '')):
                raise ValueError(f'missing exact source identity {key}')
        for key in ('board', 'variant', 'rounds', 'sync_modes'):
            if done.get(key) != header[key]:
                raise ValueError('AB_DONE identity does not match AB_READY')
        if done.get('hardware_measurement') != 'COMPLETE':
            raise ValueError('capture has no successful completion marker')
        scenes = ('dashboard', 'changing_rgb565', 'fixed_flash_off') + (('fixed_flash_warm_cache',) if native else ())
        expected = {(mode, scene, round_) for mode in MODES for scene in scenes for round_ in range(1, rounds + 1)}
        for tag in ('AB_RESULT', 'AB_TIMING', 'AB_REFRESH'):
            keys = [phase_key(row) for row in records[tag]]
            if len(keys) != len(set(keys)) or set(keys) != expected:
                raise ValueError(f'{tag}: missing or duplicate mode/scene/round rows')
        ordered_phases = [(mode, scene, n) for n in range(1, rounds + 1) for scene in scenes
                          for mode in (MODES if n % 2 else tuple(reversed(MODES)))]
        if [phase_key(row) for row in records['AB_RESULT']] != ordered_phases:
            raise ValueError('paired mode/scene order does not match the declared alternating protocol')
        result = {phase_key(row): row for row in records['AB_RESULT']}
        timing = {phase_key(row): row for row in records['AB_TIMING']}
        refresh = {phase_key(row): row for row in records['AB_REFRESH']}
        costs = defaultdict(list)
        seen_cost = set()
        for row in records['AB_COST']:
            key = phase_key(row)
            if key not in expected or row['stage'] not in STAGES or row['kind'] not in KINDS:
                raise ValueError('AB_COST: unknown phase/stage/category')
            unique = key + (row['stage'], row['kind'])
            if unique in seen_cost:
                raise ValueError('AB_COST: duplicate phase/stage/category')
            seen_cost.add(unique)
            calls = integer(row, 'calls', 1)
            for name in ('us', 'bytes', 'pixels', 'pending', 'flags_or', 'c2m_bytes', 'm2c_bytes'):
                integer(row, name)
            if integer(row, 'errors') or row.get('inclusive') != '1':
                raise ValueError('AB_COST: failed operation or missing inclusive-time marker')
            if integer(row, 'pending') > calls or (row['kind'] != 'native_wait' and integer(row, 'pending')):
                raise ValueError('AB_COST: impossible pending-wait count')
            costs[key].append(row)
        for tag in ('AB_RESULT', 'AB_TIMING', 'AB_REFRESH', 'AB_COST'):
            for row in records[tag]:
                if any(row.get(k) != header[k] for k in ('protocol', 'board', 'variant')):
                    raise ValueError(f'{tag}: mixed identity within capture')
        if len({integer(row, 'tick_hz', 1) for row in records['AB_RESULT']}) != 1:
            raise ValueError('inconsistent RTOS tick rate within capture')
        for key in expected:
            row, t, e = result[key], timing[key], refresh[key]
            if integer(row, 'defer_empty_sync') != int(key[0] == 'deferred'):
                raise ValueError('recorded synchronization policy does not match mode')
            if integer(row, 'frames') != frames or integer(t, 'frames') != frames:
                raise ValueError('incomplete per-phase frame count')
            if row['heap_ok'] != '1' or any(integer(row, field) for field in ('turner_failed', 'failures', 'invalid_indices')):
                raise ValueError('failed heap/transfer/scanout observation')
            if row['scanout_metric'] != 'GENERATION_SELECTED_FOR_DMA':
                raise ValueError('requested DMA probe did not produce valid observations')
            selected, unselected = integer(row, 'selected', 1), integer(row, 'unselected')
            if selected + unselected != frames or selected > frames:
                raise ValueError('invalid selected/unselected cohort')
            if integer(row, 'draw_calls', 1) < frames or integer(row, 'scanout_irqs', 1) < selected:
                raise ValueError('missing panel submissions or scanout interrupts')
            scanout = number(row, 'scanout_fps', True)
            submitted = number(row, 'submitted_fps', True)
            cohort = integer(row, 'scanout_cohort_us', 1)
            produced = integer(row, 'produced_us', 1)
            if not math.isclose(scanout, selected * 1e6 / cohort, rel_tol=1e-6, abs_tol=0.0001):
                raise ValueError('scanout FPS does not match selected cohort')
            if not math.isclose(submitted, frames * 1e6 / produced, rel_tol=1e-6, abs_tol=0.0001):
                raise ValueError('submitted FPS does not match trace timing')
            if integer(t, 'produced_us', 1) != produced:
                raise ValueError('timing window does not match result')
            names = ('handoff_us', 'lock_us', 'update_us', 'refresh_us', 'timer_policy_us', 'unlock_us', 'yield_us', 'residual_us')
            if sum(integer(t, field) for field in names) != produced:
                raise ValueError('timing accounting does not reconstruct produced_us')
            if not math.isclose(number(row, 'refresh_mean_us'), integer(t, 'refresh_us') / frames, abs_tol=0.02):
                raise ValueError('refresh mean does not match timed manual calls')
            if not integer(row, 'refresh_median_us') <= integer(row, 'refresh_p95_us') <= integer(row, 'refresh_worst_us'):
                raise ValueError('invalid refresh percentiles')
            for field in ('drain_us', 'close_lock_us'):
                integer(t, field)
            if any(integer(t, field) != frames for field in ('lock_calls', 'update_calls', 'refresh_calls', 'unlock_calls')):
                raise ValueError('timing call counts do not match frames')
            if integer(t, 'yield_calls') != integer(row, 'idle_yields'):
                raise ValueError('yield count mismatch')
            if integer(e, 'manual_starts') != frames or integer(e, 'manual_ready') != frames:
                raise ValueError('manual refresh-event count does not match frames')
            automatic = integer(e, 'automatic_starts')
            if automatic != integer(e, 'automatic_ready') or integer(e, 'nested') or integer(e, 'unmatched_ready'):
                raise ValueError('unbalanced or nested refresh events')
            if integer(e, 'strict_pauses'):
                raise ValueError('automatic refresh timer was overridden')
            for field in ('manual_us', 'automatic_us', 'requests', 'requests_running', 'strict_pauses'):
                integer(e, field)
            if e.get('inclusive') != '1' or e.get('window') != 'trace_plus_drain_and_close_lock':
                raise ValueError('refresh-event timing scope not identified')
            if integer(e, 'requests_running') > integer(e, 'requests'):
                raise ValueError('impossible refresh-request count')
            if key[0] == 'eager' and integer(e, 'strict_pauses'):
                raise ValueError('timer override applied in eager mode')
            if row['scene'] != 'fixed_flash_warm_cache' and integer(row, 'cache_budget'):
                raise ValueError('warm image cache mixed into normal-scene comparison')
            if row['scene'] == 'changing_rgb565' and integer(row, 'cache_hits'):
                raise ValueError('changing frames unexpectedly used immutable-image cache')
            if row['scene'] == 'fixed_flash_warm_cache' and not (integer(row, 'cache_budget', 1) and integer(row, 'cache_hits', 1)):
                raise ValueError('warm-cache phase did not exercise cache')
        return {'path': str(path), 'header': header, 'done': done,
                'results': result, 'timing': timing, 'refresh': refresh, 'costs': dict(costs)}
    except (KeyError, ValueError) as exc:
        raise ValueError(f'{path}: {exc}') from exc


def summarize(captures):
    settings, identities, groups = {}, {}, defaultdict(list)
    for capture in captures:
        header = capture['header']
        board, variant = header['board'], header['variant']
        profile = tuple(header[field] for field in COMMON)
        if board in settings and settings[board] != profile:
            raise ValueError(f'{board}: configuration mismatch between variants')
        settings[board] = profile
        if (board, variant) in identities:
            raise ValueError(f'{board}/{variant}: duplicate complete capture')
        identities[board, variant] = header
        for key, row in capture['results'].items():
            mode, scene, round_ = key
            groups[board, variant, mode, scene].append((round_, row, capture['timing'][key], capture['refresh'][key], capture['costs'].get(key, [])))
    output = []
    for (board, variant, mode, scene), values in sorted(groups.items()):
        rows = [v[1] for v in values]
        frames = integer(rows[0], 'frames')
        ticks = {integer(row, 'tick_hz', 1) for row in rows}
        if len(ticks) != 1:
            raise ValueError(f'{board}/{variant}/{mode}: inconsistent tick rate')
        fps = statistics.median(number(row, 'scanout_fps', True) for row in rows)
        entry = {'board': board, 'variant': variant, 'sync_mode': mode, 'scene': scene,
                 'rounds': len(values), 'metric': 'generations_selected_for_dma_per_second',
                 'median_fps': fps, 'min_fps': min(float(row['scanout_fps']) for row in rows),
                 'max_fps': max(float(row['scanout_fps']) for row in rows),
                 'median_p95_refresh_us': statistics.median(integer(row, 'refresh_p95_us') for row in rows),
                 'unselected_frames': sum(integer(row, 'unselected') for row in rows),
                 'minimum_internal': min(integer(row, 'minimum_internal') for row in rows),
                 'minimum_free': min(integer(row, 'minimum_free') for row in rows),
                 'tick_hz': next(iter(ticks)), 'timing_ms_per_frame': {}, 'refresh_events': [], 'costs_by_round': []}
        for field in ('handoff_us', 'lock_us', 'update_us', 'refresh_us', 'timer_policy_us', 'unlock_us', 'yield_us', 'residual_us'):
            entry['timing_ms_per_frame'][field[:-3]] = statistics.median(integer(v[2], field) / frames / 1000 for v in values)
        entry['median_drain_ms'] = statistics.median(integer(v[2], 'drain_us') / 1000 for v in values)
        entry['median_close_lock_ms'] = statistics.median(integer(v[2], 'close_lock_us') / 1000 for v in values)
        for round_, _, _, events, costs in sorted(values):
            entry['refresh_events'].append({'round': round_, **{k: int(v) if v.isdigit() else v for k, v in events.items() if k not in ('protocol', 'board', 'variant', 'sync_mode', 'scene', 'round')}})
            entry['costs_by_round'].append({'round': round_, 'rows': [{k: int(v) if v.isdigit() else v for k, v in row.items() if k not in ('protocol', 'board', 'variant', 'sync_mode', 'scene', 'round')} for row in costs]})
        baseline = groups.get((board, 'official', mode, scene))
        entry['gain_vs_official_same_sync_mode_percent'] = None
        if baseline:
            if {integer(v[1], 'tick_hz', 1) for v in baseline} != ticks:
                raise ValueError(f'{board}/{mode}/{scene}: tick mismatch between variants')
            base = statistics.median(float(v[1]['scanout_fps']) for v in baseline)
            entry['gain_vs_official_same_sync_mode_percent'] = (fps / base - 1) * 100
        legacy = groups.get((board, variant, 'eager', scene))
        entry['deferred_vs_eager_percent'] = None
        if mode == 'deferred' and legacy:
            base = statistics.median(float(v[1]['scanout_fps']) for v in legacy)
            entry['deferred_vs_eager_percent'] = (fps / base - 1) * 100
            previous = {v[0]: v[1] for v in legacy}
            entry['paired_mode_gains_percent'] = [{'round': n, 'gain': (float(row['scanout_fps']) / float(previous[n]['scanout_fps']) - 1) * 100} for n, row, *_ in sorted(values)]
        output.append(entry)
    return {'protocol': 9,
            'notes': ['DMA generation selection is not an optical panel measurement.',
                      'PPA submit/native wait/copy/cache times are inclusive and can overlap: never sum category times.',
                      'Copy payload and cache request spans do not measure total physical bus traffic.',
                      'Only matching board, scene and timer mode are compared. Warm immutable-image caching remains separate.',
                      'Eager/deferred diagnostic counts include the recorded drain and closing-lock window; submitted throughput excludes them.',
                      'V9 uses paired modes and a different trace length from V7; do not pool their raw rows.'],
            'captures': [{'path': c['path'], **c['header']} for c in captures], 'results': output}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs', nargs='+', type=Path)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    report = summarize([parse(path) for path in args.logs])
    rendered = json.dumps(report, indent=2) + '\n'
    if args.out:
        args.out.write_text(rendered)
    print(rendered)
