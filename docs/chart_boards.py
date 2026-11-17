#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 youkorr
# Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
"""Draw docs/cartes.svg from docs/mesures-cartes.csv.

Images per second measured on the household's boards (an ESP32-P4 Waveshare 7B
and an M5Stack Tab5): against Espressif's esp_lvgl_adapter through the A/B
bench of PR #818 (espressif/pr818-ab, the same firmware built on both
adapters), and against ESPHome's LVGL rotation, a dashboard redrawn whole
(lvgl_bench's ms per frame, as 1000 / ms). Standard library only:

    python3 docs/chart_boards.py
"""
import csv
import math
import os
from xml.sax.saxutils import escape

HERE = os.path.dirname(os.path.abspath(__file__))
TITLES = {
    "espressif": "Contre l'adaptateur d'Espressif (banc A/B de la PR #818, même firmware sur les deux)",
    "esphome": "Contre la rotation de LVGL sous ESPHome",
}


def fr(v, digits=1):
    return f"{v:.{digits}f}".replace(".", ",")


def main():
    with open(os.path.join(HERE, "mesures-cartes.csv"), newline="") as f:
        rows = list(csv.DictReader(f))
    for r in rows:
        r["reference"] = float(r["reference"])
        r["ppa_alternative"] = float(r["ppa_alternative"])
    groups = [g for g in TITLES if any(r["groupe"] == g for r in rows)]

    W, left, right = 1000, 300, 40
    top, bar, gap, case_gap, title_h, group_gap = 96, 16, 3, 16, 30, 22
    case_h = 2 * bar + gap
    H = top + sum(title_h + sum(case_h + case_gap for r in rows if r["groupe"] == g) + group_gap
                  for g in groups) + 20
    # Room past the longest bar for its label, in steps of 5 /s.
    vmax = 5.0 * math.ceil(max(r[k] for r in rows for k in ("reference", "ppa_alternative")) * 1.45 / 5)
    x = lambda v: left + (W - left - right) * v / vmax

    out = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}" '
           f'font-family="-apple-system, Segoe UI, Helvetica, Arial, sans-serif" role="img" '
           f'aria-label="Images par seconde mesurées sur les cartes : PPA alternative contre Espressif et LVGL">',
           """<style>
  .bg { fill: #ffffff } .fg { fill: #1f2328 } .mute { fill: #59636e } .grid { stroke: #d1d9e0 }
  .ref { fill: #e0823d } .lvgl { fill: #b6bec8 } .ppa { fill: #1a6fd1 } .gain { fill: #1a7f37 }
  @media (prefers-color-scheme: dark) {
    .bg { fill: #0d1117 } .fg { fill: #e6edf3 } .mute { fill: #9198a1 } .grid { stroke: #3d444d }
    .ref { fill: #f0883e } .lvgl { fill: #5a626c } .ppa { fill: #388bfd } .gain { fill: #3fb950 }
  }
</style>""",
           f'<rect class="bg" width="{W}" height="{H}"/>',
           '<text class="fg" x="24" y="38" font-size="22" font-weight="600">'
           'PPA alternative, mesuré sur les cartes</text>',
           '<text class="mute" x="24" y="62" font-size="13">Images par seconde, plus long est meilleur. '
           'Waveshare 7B (1024x600, 180°) et M5Stack Tab5 (1280x720, 270°), ESP-IDF 5.5.6.</text>']
    y = top
    for g in groups:
        out.append(f'<text class="fg" x="24" y="{y + 16}" font-size="14" font-weight="600">'
                   f'{escape(TITLES[g])}</text>')
        y += title_h
        g_top = y
        cases = [r for r in rows if r["groupe"] == g]
        ref_class = "ref" if g == "espressif" else "lvgl"
        g_bottom = g_top + len(cases) * (case_h + case_gap) - case_gap
        for v in range(0, int(vmax) + 1, 5):
            out.append(f'<line class="grid" x1="{x(v):.1f}" x2="{x(v):.1f}" y1="{g_top - 4}" '
                       f'y2="{g_bottom + 4}"/>')
        for r in cases:
            gain = r["ppa_alternative"] / r["reference"]
            # Against Espressif in percent, as the README and the PR give it.
            # Equal within half a percent is the panel's own rate in both:
            # neither can show more frames than the panel scans out.
            label = ("=" if abs(gain - 1) < 0.005
                     else f"+{fr((gain - 1) * 100, 0)} %" if gain < 1.5
                     else f"{fr(gain, 2)}×")
            out.append(f'<text class="fg" x="{left - 14}" y="{y + case_h / 2 - 3:.1f}" font-size="13" '
                       f'text-anchor="end">{escape(r["cas"])}</text>')
            out.append(f'<text class="gain" x="{left - 14}" y="{y + case_h / 2 + 14:.1f}" font-size="14" '
                       f'font-weight="700" text-anchor="end">{label}</text>')
            for i, (cls, key, name) in enumerate(((ref_class, "reference", r["reference_nom"]),
                                                  ("ppa", "ppa_alternative", "PPA alternative"))):
                by = y + i * (bar + gap)
                out.append(f'<rect class="{cls}" x="{left}" y="{by}" width="{x(r[key]) - left:.1f}" '
                           f'height="{bar}" rx="2"/>')
                out.append(f'<text class="mute" x="{x(r[key]) + 5:.1f}" y="{by + bar - 3}" font-size="11">'
                           f'{fr(r[key])} /s · {escape(name)}</text>')
            y += case_h + case_gap
        y += group_gap
    for v in range(0, int(vmax) + 1, 5):
        out.append(f'<text class="mute" x="{x(v):.1f}" y="{H - 14}" font-size="11" '
                   f'text-anchor="middle">{v} /s</text>')
    out.append("</svg>")
    with open(os.path.join(HERE, "cartes.svg"), "w") as f:
        f.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
