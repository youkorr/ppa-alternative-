#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 youkorr
# SPDX-License-Identifier: MIT
"""Validate A/B serial results and compare medians for matching board/scenes."""
import argparse
from collections import defaultdict
import json
from pathlib import Path
import re
import statistics

FIELDS = re.compile(r'(\w+)=([^\s]+)')


def parse(path):
    header = None
    results = []
    done = False
    for line in path.read_text(errors='replace').splitlines():
        if 'AB_READY ' in line:
            if header:
                raise ValueError(f'{path}: multiple boots in one capture; split them before comparison')
            header = dict(FIELDS.findall(line.split('AB_READY ', 1)[1]))
        elif 'AB_RESULT ' in line:
            results.append(dict(FIELDS.findall(line.split('AB_RESULT ', 1)[1])))
        elif 'AB_DONE ' in line:
            done = True
    if not header or not done:
        raise ValueError(f'{path}: complete AB_READY … AB_DONE capture required')
    stock_ppa = header['variant'] == 'official_ppa1'
    if int(header['draw_units']) != (1 if stock_ppa else 2) or int(header['ppa_draw_requested']) != int(stock_ppa):
        raise ValueError(f'{path}: draw-unit/PPA configuration does not match the named variant')
    expected_scenes = ['dashboard', 'changing_rgb565', 'fixed_flash_off']
    if header['variant'].startswith('native'):
        expected_scenes.append('fixed_flash_warm_cache')
    expected = {(scene, str(n)) for scene in expected_scenes for n in range(1, int(header['rounds']) + 1)}
    observed = {(r['scene'], r['round']) for r in results}
    if observed != expected or len(observed) != len(results):
        raise ValueError(f'{path}: missing or duplicate scene/round rows')
    for r in results:
        if r['board'] != header['board'] or r['variant'] != header['variant']:
            raise ValueError(f'{path}: mixed board/variant data')
        if r['heap_ok'] != '1' or r['turner_failed'] != '0' or r['failures'] != '0':
            raise ValueError(f'{path}: failed heap or transfer, not a valid speed result')
        if int(r['frames']) != int(header['frames']):
            raise ValueError(f'{path}: incomplete frame count')
        if header['dpi_probe'] == '1':
            if r['scanout_metric'] != 'GENERATION_SELECTED_FOR_DMA' or int(r['invalid_indices']):
                raise ValueError(f'{path}: requested scanout probe did not produce valid observations')
            if not 0 < int(r['selected']) <= int(r['frames']):
                raise ValueError(f'{path}: impossible selected-generation count')
    return header, results


def summarize(captures):
    common = {}
    grouped = defaultdict(list)
    metadata = {}
    for header, rows in captures:
        board, variant = header['board'], header['variant']
        settings = tuple(header[k] for k in ('logical', 'rotation', 'panel_buffers', 'cpu_mhz', 'psram_mhz', 'l2_kib', 'frames', 'rounds', 'dashboard_full', 'dpi_probe'))
        if board in common and common[board] != settings:
            raise ValueError(f'{board}: configuration mismatch; results must not be compared')
        common[board] = settings
        if (board, variant) in metadata:
            raise ValueError(f'{board}/{variant}: duplicate complete capture')
        metadata[board, variant] = header
        for row in rows:
            grouped[board, variant, row['scene']].append(row)
    output = []
    for (board, variant, scene), rows in sorted(grouped.items()):
        metric = 'scanout_fps' if all(r['scanout_metric'] == 'GENERATION_SELECTED_FOR_DMA' for r in rows) else 'submitted_fps'
        entry = {'board': board, 'variant': variant, 'scene': scene, 'rounds': len(rows),
                 'metric': metric, 'median_fps': statistics.median(float(r[metric]) for r in rows),
                 'min_fps': min(float(r[metric]) for r in rows),
                 'max_fps': max(float(r[metric]) for r in rows),
                 'median_refresh_us': statistics.median(int(r['refresh_median_us']) for r in rows),
                 'median_p95_refresh_us': statistics.median(int(r['refresh_p95_us']) for r in rows),
                 'unselected_frames': sum(int(r['unselected']) for r in rows),
                 'free_before_min': min(int(r['free_before']) for r in rows),
                 'internal_before_min': min(int(r['internal_before']) for r in rows),
                 'minimum_free': min(int(r['minimum_free']) for r in rows),
                 'minimum_internal': min(int(r['minimum_internal']) for r in rows),
                 'largest_internal_before_min': min(int(r['largest_internal_before']) for r in rows),
                 'largest_internal_after_min': min(int(r['largest_internal_after']) for r in rows),
                 'native_stats': [{'round': int(r['round']), **{k: int(r[k]) for k in
                                   ('native_stats', 'tile_internal', 'tiles_in_turn', 'batches', 'waits', 'bytes_turned')}} for r in rows],
                 'tick_hz': sorted({int(r['tick_hz']) for r in rows}),
                 'cache_bytes_max': max(int(r['cache_bytes']) for r in rows)}
        if len(entry['tick_hz']) != 1:
            raise ValueError(f'{board}/{variant}: inconsistent RTOS tick rate')
        baseline_choices = [(name, grouped[board, name, scene]) for name in ('official', 'official_ppa1')
                            if (board, name, scene) in grouped]
        if baseline_choices:
            baseline_name, baseline = max(baseline_choices, key=lambda pair: statistics.median(float(r[metric]) for r in pair[1]))
            baseline_metric = 'scanout_fps' if all(r['scanout_metric'] == 'GENERATION_SELECTED_FOR_DMA' for r in baseline) else 'submitted_fps'
            if metric != baseline_metric or {int(r['tick_hz']) for r in baseline} != set(entry['tick_hz']):
                raise ValueError(f'{board}/{scene}: instrumentation/tick mismatch')
            base = statistics.median(float(r[metric]) for r in baseline)
            entry['gain_vs_official_percent'] = (entry['median_fps'] / base - 1) * 100
            entry['best_official_baseline'] = baseline_name
        else:
            entry['gain_vs_official_percent'] = None
            entry['best_official_baseline'] = None
        output.append(entry)
    return {'note': 'scanout_fps counts generations selected for DMA, not optical panel measurements; submitted_fps must not be relabeled as displayed FPS',
            'results': output, 'captures': [{'board': b, 'variant': v, **h} for (b, v), h in sorted(metadata.items())]}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs', nargs='+', type=Path)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    report = summarize([parse(p) for p in args.logs])
    text = json.dumps(report, indent=2) + '\n'
    if args.out:
        args.out.write_text(text)
    print(text)
