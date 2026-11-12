# SPDX-FileCopyrightText: 2026 youkorr
# SPDX-License-Identifier: MIT
"""Compare the median FPS of genuine PR818_RESULT lines in saved serial logs."""
import argparse
import csv
from collections import defaultdict
from pathlib import Path
import re
from statistics import median
import warnings

COMMITS = {'before': 'ba1ed66c40c840cd1b2dbe9062760c2c86262b5a',
           'after': '7965432ed3220ab29a2b44c84d78f341d4185224',
           'after_v2': '9db972e7008d4c5adab0fde59dc3b0b1343dac4c'}
ANSI = re.compile(r'\x1b\[[0-?]*[ -/]*[@-~]')


def parse(paths):
    rows = []
    settings = {}
    for path in paths:
        text = ANSI.sub('', path.read_text(errors='replace'))
        started = set()
        if 'PR818 board comparison:PASS' not in text or 'PR818 board comparison:FAIL' in text:
            raise ValueError(f'{path}: résultat Unity PASS manquant ou échec présent')
        if 'PR818_DONE' not in text:
            raise ValueError(f'{path}: journal incomplet, PR818_DONE manquant')
        panics = text.count('Guru Meditation Error')
        if panics:
            warnings.warn(f'{path}: {panics} plantage(s) présent(s) ; les FPS de la session complète ne valident pas la stabilité')
        for line in text.splitlines():
            if 'PR818_START ' in line:
                fields = dict(re.findall(r'(\w+)=([^\s]+)', line.split('PR818_START ', 1)[1]))
                if fields.get('commit') != COMMITS.get(fields.get('revision')):
                    raise ValueError(f'{path}: commit inattendu')
                key = fields['board']
                config = tuple(fields[k] for k in ('logical', 'cpu_mhz', 'l2_kib', 'psram_mhz', 'sensor_capture'))
                if key in settings and settings[key] != config:
                    raise ValueError(f'{path}: réglages différents pour {key}')
                settings[key] = config
                started.add((fields['board'], fields['revision']))
                if fields['revision'] == 'after_v2':
                    if panics or text.count('adapter render threads can be stopped repeatedly:PASS') != 16:
                        raise ValueError(f'{path}: les 16 cycles de redémarrage doivent réussir sans plantage')
                    if 'PR818_RESTART_OK count=16' not in text:
                        raise ValueError(f'{path}: confirmation des 16 cycles manquante')
                    if 'out.pic_w/h mismatch with out.buffer_size' in text:
                        raise ValueError(f'{path}: erreur de géométrie PPA encore présente')
            # PR818_DONE also mentions "PR818_RESULT line" in its human text.
            # A result record starts with the board field, unlike that message.
            if 'PR818_RESULT board=' not in line:
                continue
            row = dict(re.findall(r'(\w+)=([^\s]+)', line.split('PR818_RESULT ', 1)[1]))
            if (row['board'], row['revision']) not in started:
                raise ValueError(f'{path}: PR818_START manquant avant les résultats')
            if int(row['failed']) or int(row['frames']) == 0:
                raise ValueError(f'{path}: PPA bloqué ou aucune image, résultat non comparable')
            if row['scene'] == 'fixed_on' and int(row['cache_hits']) == 0:
                raise ValueError(f'{path}: le cache n’a pas été utilisé')
            if row['scene'].startswith('moving') and int(row['cache_hits']) != 0:
                raise ValueError(f'{path}: une image modifiable est entrée dans le cache')
            if row['scene'] == 'moving_native' and int(row['native']) == 0:
                raise ValueError(f'{path}: chemin des images natives non utilisé')
            rows.append(row)
    return rows


def compare(rows):
    groups = defaultdict(list)
    for row in rows:
        groups[(row['board'], row['revision'], row['scene'])].append(row)
    values = {}
    for key, group in sorted(groups.items()):
        if len(group) < 3:
            raise ValueError(f'{key}: trois répétitions sont nécessaires')
        values[key] = median(float(row['fps']) for row in group)
        print(f'{key[0]:13} {key[1]:6} {key[2]:14} : {values[key]:7.3f} images/s (médiane de {len(group)} résultats)')
    print('\nComparaisons :')
    comparisons = []
    for board in sorted({key[0] for key in groups}):
        for source, target in (('before', 'after'), ('before', 'after_v2'), ('after', 'after_v2')):
            for scene in ('ui', 'fixed_off', 'moving_off', 'moving_native'):
                a, b = (board, source, scene), (board, target, scene)
                if a in values and b in values:
                    gain = (values[b] / values[a] - 1) * 100
                    print(f'{board} / {scene} : {source} → {target}, {gain:+.1f} %')
                    comparisons.append((board, scene, f'{source}_to_{target}', values[a], values[b], gain))
        for revision in ('after', 'after_v2'):
            for off, on in (('fixed_off', 'fixed_on'), ('moving_off', 'moving_on')):
                a, b = (board, revision, off), (board, revision, on)
                if a in values and b in values:
                    gain = (values[b] / values[a] - 1) * 100
                    print(f'{board} / {revision} / {off} → {on} : cache désactivé → activé, {gain:+.1f} %')
                    comparisons.append((board, off, f'{revision}_cache', values[a], values[b], gain))
    return comparisons


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs', type=Path, nargs='+')
    parser.add_argument('--csv', type=Path)
    args = parser.parse_args()
    try:
        results = compare(parse(args.logs))
    except (ValueError, KeyError) as error:
        parser.error(str(error))
    if args.csv:
        with args.csv.open('w', newline='') as out:
            writer = csv.writer(out)
            writer.writerow(['board', 'scene', 'comparison', 'before_fps', 'after_fps', 'fps_gain_percent'])
            writer.writerows(results)


if __name__ == '__main__':
    main()
