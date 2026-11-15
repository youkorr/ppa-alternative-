#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 youkorr
# Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
"""Les journaux des kits A/B de la PR 818, scène par scène.

    python3 test_pr818/depouiller.py                 les images par seconde
    python3 test_pr818/depouiller.py --couts         et où part le temps (V8, V9)
    python3 test_pr818/depouiller.py --moteur        et ce que le moteur de la PR a fait

Lit tous les fichiers .txt du dossier (ou ceux donnés en argument). Les lignes
AB_RESULT donnent les images par seconde de chaque passage ; AB_COST, les
copies, les synchronisations de cache, les transferts du PPA et les attentes.
Les temps d'AB_COST sont inclusifs : ils ne s'additionnent pas.
"""
import glob
import os
import re
import statistics as st
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))


def fields(line):
    return dict(re.findall(r"(\w+)=(\S+)", line))


def mode_of(kv):
    return kv.get("timer_mode") or kv.get("sync_mode") or "-"


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    costs = "--couts" in sys.argv
    engine = "--moteur" in sys.argv
    files = args or sorted(glob.glob(os.path.join(HERE, "*.txt")))
    for path in files:
        results, cost, native, cpu = defaultdict(list), defaultdict(list), defaultdict(list), defaultdict(list)
        camera, camera_ready = defaultdict(list), None
        frames = 64
        with open(path, errors="replace") as f:
            for line in f:
                if "AB_RESULT" in line:
                    kv = fields(line)
                    frames = int(kv["frames"])
                    results[(mode_of(kv), kv["scene"])].append(kv)
                elif "AB_CAMERA_READY" in line:
                    camera_ready = fields(line)
                elif "AB_CAMERA protocol" in line:
                    kv = fields(line)
                    camera[(mode_of(kv), kv["scene"])].append(kv)
                elif "AB_CPU protocol" in line:
                    kv = fields(line)
                    cpu[(mode_of(kv), kv["scene"])].append(kv)
                elif "AB_NATIVE" in line:
                    kv = fields(line)
                    native[(mode_of(kv), kv["scene"])].append(kv)
                elif "AB_COST" in line:
                    kv = fields(line)
                    cost[(mode_of(kv), kv["scene"], kv.get("stage", "-"), kv["kind"])].append(kv)
        print("==", os.path.basename(path))
        if camera_ready:
            print(f"   caméra : {camera_ready.get('status')}, {camera_ready.get('sensor')} "
                  f"{camera_ready.get('width')}x{camera_ready.get('height')}")
        for (mode, scene), rows in results.items():
            fps = [float(r["submitted_fps"]) for r in rows]
            med = st.median(float(r["refresh_median_us"]) for r in rows) / 1000
            p95 = st.median(float(r["refresh_p95_us"]) for r in rows) / 1000
            failed = sum(int(r["failures"]) for r in rows)
            load = ""
            if cpu.get((mode, scene)):
                c = cpu[(mode, scene)]
                busy = st.median(float(r["busy_pct"]) for r in c)
                work = st.median(float(r["busy_us_per_frame"]) for r in c) / 1000
                load = f", processeur occupé {busy:4.1f} % ({work:5.1f} ms de travail par image, deux cœurs)"
            print(f"   {mode:9s} {scene:23s} {st.median(fps):6.2f} images/s  "
                  f"(rafraîchissement médian {med:6.1f} ms, p95 {p95:6.1f} ms, {len(rows)} passages, {failed} échec)"
                  + load)
            if camera.get((mode, scene)):
                c = camera[(mode, scene)]
                print(f"      {'':9s} {'':23s} caméra : {st.median(float(r['camera_fps']) for r in c):5.2f} images/s "
                      f"livrées, {st.median(float(r['shown_fps']) for r in c):5.2f} affichées, "
                      f"{st.mean(float(r['skipped']) for r in c):4.1f} sautées sur {frames}")
        if engine:
            for (mode, scene), rows in native.items():
                tiles = st.mean(float(r["tiles"]) for r in rows) / frames
                read = st.mean(float(r["tiles"]) - float(r["tiles_unread"]) for r in rows) / frames
                other = st.mean(float(r["tiles_other_core"]) for r in rows) / frames
                cores = ",".join(sorted({r["helper_cores"] for r in rows}))
                print(f"      {mode:9s} {scene:23s} {tiles:5.1f} tuiles/image, {read:5.1f} relues, "
                      f"{other:5.1f} sur l'autre cœur (tâches sur les cœurs {cores}), "
                      f"{st.mean(float(r['sync_bytes_copied']) for r in rows) / frames / 1024:6.1f} Kio recopiés")
        if not costs:
            continue
        for (mode, scene, stage, kind), rows in cost.items():
            calls = st.mean(float(r["calls"]) for r in rows) / frames
            ms = st.mean(float(r["us"]) for r in rows) / frames / 1000
            kib = st.mean(float(r["bytes"]) for r in rows) / frames / 1024
            if calls == 0 and ms == 0:
                continue
            print(f"      {mode:9s} {scene:23s} {stage:10s} {kind:12s} {calls:6.1f} appels, {ms:7.2f} ms, "
                  f"{kib:8.1f} Kio par image")


if __name__ == "__main__":
    main()
