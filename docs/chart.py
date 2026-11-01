#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 youkorr
# Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
"""Draw docs/avance.svg from docs/p4sim-resultats.csv.

The figures are test/p4sim/p4sim.py's (a simulated ESP32-P4 at 360 MHz,
middle assumptions), not a board's. Standard library only:

    python3 docs/chart.py
"""
import csv
import os
from xml.sax.saxutils import escape

HERE = os.path.dirname(os.path.abspath(__file__))
COLS = ["lvgl_esphome", "esp_lvgl_adapter", "ppa_1_coeur", "ppa_2_coeurs"]
NAMES = ["LVGL d'ESPHome / esp_lvgl_port", "esp_lvgl_adapter (Espressif)",
         "PPA alternative, 1 cœur", "PPA alternative, 2 cœurs"]
CLASSES = ["s0", "s1", "s2", "s3"]


def fr(v, digits=1):
    return f"{v:.{digits}f}".replace(".", ",")


def main():
    with open(os.path.join(HERE, "p4sim-resultats.csv"), newline="") as f:
        rows = list(csv.DictReader(f))
    for r in rows:
        for c in COLS:
            r[c] = float(r[c])

    W, left, right = 920, 170, 175
    top, legend_h, bar, gap, group_gap = 92, 34, 13, 3, 22
    group_h = 4 * bar + 3 * gap
    plot_top = top + legend_h
    H = plot_top + len(rows) * (group_h + group_gap) + 40
    vmax = 110.0
    x = lambda v: left + (W - left - right) * v / vmax

    out = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" width="{W}" height="{H}" '
           f'font-family="-apple-system, Segoe UI, Helvetica, Arial, sans-serif" role="img" '
           f'aria-label="Millisecondes par image : PPA alternative contre Espressif et LVGL">',
           """<style>
  .bg { fill: #ffffff } .fg { fill: #1f2328 } .mute { fill: #59636e } .grid { stroke: #d1d9e0 }
  .s0 { fill: #b6bec8 } .s1 { fill: #e0823d } .s2 { fill: #6cb4ee } .s3 { fill: #1a6fd1 } .gain { fill: #1a7f37 }
  @media (prefers-color-scheme: dark) {
    .bg { fill: #0d1117 } .fg { fill: #e6edf3 } .mute { fill: #9198a1 } .grid { stroke: #3d444d }
    .s0 { fill: #5a626c } .s1 { fill: #f0883e } .s2 { fill: #79c0ff } .s3 { fill: #388bfd } .gain { fill: #3fb950 }
  }
</style>""",
           f'<rect class="bg" width="{W}" height="{H}"/>',
           '<text class="fg" x="24" y="38" font-size="22" font-weight="600">'
           'PPA alternative : l\'avance sur Espressif et LVGL</text>',
           '<text class="mute" x="24" y="62" font-size="13">Millisecondes par image, plus court est meilleur. '
           'ESP32-P4 à 360 MHz simulé (test/p4sim), pas encore mesuré sur une carte.</text>']
    lx = 24
    for i, n in enumerate(NAMES):
        out.append(f'<rect class="{CLASSES[i]}" x="{lx}" y="{top - 2}" width="12" height="12" rx="2"/>')
        out.append(f'<text class="mute" x="{lx + 17}" y="{top + 8}" font-size="12">{escape(n)}</text>')
        lx += 34 + 6.6 * len(n)
    plot_bottom = H - 40
    for v in range(0, int(vmax) + 1, 20):
        out.append(f'<line class="grid" x1="{x(v):.1f}" x2="{x(v):.1f}" y1="{plot_top}" y2="{plot_bottom}"/>')
        out.append(f'<text class="mute" x="{x(v):.1f}" y="{plot_bottom + 18}" font-size="11" '
                   f'text-anchor="middle">{v} ms</text>')
    y = plot_top + 8
    for r in rows:
        out.append(f'<text class="fg" x="{left - 12}" y="{y + group_h / 2 + 4:.1f}" font-size="13" '
                   f'text-anchor="end">{escape(r["cas"])}</text>')
        for i, c in enumerate(COLS):
            by = y + i * (bar + gap)
            out.append(f'<rect class="{CLASSES[i]}" x="{left}" y="{by}" width="{x(r[c]) - left:.1f}" '
                       f'height="{bar}" rx="2"/>')
            out.append(f'<text class="mute" x="{x(r[c]) + 5:.1f}" y="{by + bar - 2}" font-size="10.5">'
                       f'{fr(r[c])}</text>')
        g_esp = r["esp_lvgl_adapter"] / r["ppa_2_coeurs"]
        g_lvgl = r["lvgl_esphome"] / r["ppa_2_coeurs"]
        gx = W - right + 18
        out.append(f'<text class="gain" x="{gx}" y="{y + group_h / 2 - 3:.1f}" font-size="15" font-weight="700">'
                   f'{fr(g_esp)}× <tspan class="mute" font-size="11" font-weight="400">vs Espressif</tspan></text>')
        out.append(f'<text class="mute" x="{gx}" y="{y + group_h / 2 + 14:.1f}" font-size="11">'
                   f'{fr(g_lvgl)}× vs LVGL</text>')
        y += group_h + group_gap
    out.append("</svg>")
    with open(os.path.join(HERE, "avance.svg"), "w") as f:
        f.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
