#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 youkorr
# Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
"""The bench as an ESP32-P4 would run it, with no board.

The PC bench (`make bench`) times x86 code on a PC's memory, which says how
many passes each method makes and little about a P4. This builds the same
code for the P4's own processor (RISC-V RV32IMAFC, -O2, as ESP-IDF builds
it), runs it in QEMU under p4cache.c, which counts every instruction and
every line the P4's caches would move to and from the PSRAM, and turns those
counts into time with figures MEASURED ON THE BOARDS:

  - the PSRAM: a memcpy of 1280x620 RGB565 from PSRAM to PSRAM took 32 to
    35 ms on the Tab5 (esp-video's CLAUDE.md, portall's canvas copy). The
    same memcpy runs here, so the cost of one PSRAM line is what makes the
    simulated copy take that long -- with the display scanning out of the
    same PSRAM meanwhile, as it does on a board;
  - the PPA's turn: 14865 us for a whole 1024x600 RGB565 picture, and the
    display's copy of it 4099 us (18964 us a draw in all), from a Waveshare
    7B's own log (portall, rotation 180).

Two figures are NOT measured and are assumptions, printed with the result:
cycles per instruction outside a cache miss (1.0, the P4's core issues one
instruction a cycle at best), and a line from internal RAM or from the L2
(10 and 20 cycles). The result is printed for a range of them.

    python3 test/p4sim/p4sim.py --lvgl /path/to/lvgl-9.5.0

Needs riscv64-unknown-elf-gcc, picolibc-riscv64-unknown-elf and
qemu-system-riscv32 with plugins (Ubuntu: gcc-riscv64-unknown-elf
picolibc-riscv64-unknown-elf qemu-system-misc). QEMU's qemu-plugin.h is
fetched from QEMU's own repository for the installed version, since it is
GPL and not copied here. A run takes a few tens of minutes.
"""
import argparse
import glob
import hashlib
import os
import re
import subprocess
import sys
import urllib.request
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
TEST = os.path.dirname(HERE)
ORTHO = os.path.join(TEST, "..", "components", "lvgl_ortho")
CC = "riscv64-unknown-elf-gcc"
ARCH = ["-march=rv32imafc", "-mabi=ilp32f", "-O2", "--specs=picolibc.specs"]
LINK = ["--oslib=semihost", "--crt0=semihost",
        "-Wl,--defsym=__flash=0x80000000", "-Wl,--defsym=__flash_size=0x01000000",
        "-Wl,--defsym=__ram=0x81000000", "-Wl,--defsym=__ram_size=0x01000000",
        "-Wl,--defsym=__stack_size=0x20000"]

MHZ = 360                      # CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_360, the boards'
COPY_MS = (32.0, 35.0)         # Tab5, 1280x620 RGB565 memcpy PSRAM to PSRAM
PPA_NS_PX = 14865e3 / (1024 * 600)        # Waveshare 7B, rotate whole panel
DISPLAY_NS_PX = (18964 - 14865) * 1e3 / (1024 * 600)   # its copy to the panel

BOARDS = [  # name, panel, phase of the UI, phase of the picture
    ("Tab5, 720x1280 at 270", 720 * 1280, 20, 50),
    ("Guition JC8012P4A1, 800x1280 at 90", 800 * 1280, 30, 60),
    ("Waveshare 7B, 1024x600 at 180", 1024 * 600, 40, 70),
]


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        sys.exit(f"failed: {' '.join(cmd[:3])}...\n{r.stdout[-2000:]}{r.stderr[-2000:]}")
    # QEMU's semihosting writes the guest's printf to stderr.
    return r.stdout + r.stderr


def build(lvgl, work):
    os.makedirs(work, exist_ok=True)
    # LVGL patched as the tests patch it (test/Makefile).
    run(["make", "-s", "-C", TEST, f"LVGL={lvgl}", f"BUILD={work}/host", f"{work}/host/lvgl-src/.patched"])
    src = f"{work}/host/lvgl-src"
    inc = ["-DLV_CONF_INCLUDE_SIMPLE", f"-I{TEST}", f"-I{ORTHO}", f"-I{src}"]
    objs = []

    def one(c, extra=(), name=None):
        o = os.path.join(work, "rv32", name or hashlib.md5(c.encode()).hexdigest()[:12] + ".o")
        os.makedirs(os.path.dirname(o), exist_ok=True)
        run([CC, *ARCH, *inc, *extra, "-w", "-c", c, "-o", o])
        return o

    files = sorted(glob.glob(f"{src}/src/**/*.c", recursive=True))
    with ThreadPoolExecutor(os.cpu_count()) as ex:
        objs += list(ex.map(one, files))
    stub = os.path.join(TEST, "p4stub")
    # The tile asks for internal RAM as it does on a board: ESP_PLATFORM,
    # with p4sim/stub's heap_caps. LVGL's own Kconfig header is left out.
    objs.append(one(os.path.join(ORTHO, "lv_draw_ortho.c"),
                    ["-DESP_PLATFORM", "-DLV_KCONFIG_IGNORE", f"-I{HERE}/stub"], "ortho.o"))
    objs.append(one(os.path.join(ORTHO, "lv_draw_ortho_video.c"), [f"-I{stub}"], "video.o"))
    # P4SIM: the fake PPA marks its copying as DMA, kept out of the counts.
    objs.append(one(os.path.join(stub, "p4fake.c"), [f"-I{stub}", "-DP4SIM"], "p4fake.o"))
    objs.append(one(os.path.join(ORTHO, "lv_draw_ortho_ppa.c"), [f"-I{stub}"], "ppa.o"))
    objs.append(one(os.path.join(HERE, "p4bench.c"), [f"-I{stub}"], "p4bench.o"))
    # GCC would turn their loops back into calls to themselves.
    objs.append(one(os.path.join(HERE, "string.c"), ["-fno-tree-loop-distribute-patterns", "-fno-builtin"],
                    "string.o"))
    elf = os.path.join(work, "p4bench.elf")
    run([CC, *ARCH, *LINK, *objs, "-lm", "-o", elf])

    ver = re.search(r"version (\d+\.\d+\.\d+)", run(["qemu-system-riscv32", "--version"])).group(1)
    hdr = os.path.join(work, "qemu-plugin.h")
    if not os.path.exists(hdr):
        url = f"https://gitlab.com/qemu-project/qemu/-/raw/v{ver}/include/qemu/qemu-plugin.h"
        urllib.request.urlretrieve(url, hdr)
    so = os.path.join(work, "p4cache.so")
    run(["cc", "-O2", "-fPIC", "-shared", f"-I{work}", os.path.join(HERE, "p4cache.c"), "-o", so])
    return elf, so


def simulate(elf, so, out, l1=64, l1ways=4, l2=128):
    log = run(["qemu-system-riscv32", "-M", "virt", "-cpu", "rv32", "-m", "1G", "-nographic",
               "-bios", "none", "-kernel", elf, "-semihosting-config", "enable=on,target=native",
               "-plugin", f"{so},out={out},l1={l1},l1ways={l1ways},l2={l2}", "-d", "plugin"])
    return log


def read(path):
    with open(path) as f:
        head = f.readline().split()
        return {int(r[0]): dict(zip(head[1:], map(int, r[1:]))) for r in (l.split() for l in f)}


def cycles(k, cpi, c_int, c_l2, c_line):
    return (k["insns"] * cpi + k["l1_miss_internal"] * c_int + k["l2_hit"] * c_l2
            + (k["psram_fill"] + k["psram_writeback"]) * c_line)


def model(p, cpi, c_int, c_l2, copy_ms):
    """Cycles a PSRAM line costs, so the simulated memcpy takes copy_ms."""
    k = p[10]
    lines = k["psram_fill"] + k["psram_writeback"]
    rest = cycles(k, cpi, c_int, c_l2, 0)
    return max(0.0, (copy_ms * 1e-3 * MHZ * 1e6 - rest) / lines)


def ms(k, cpi, c_int, c_l2, c_line):
    return cycles(k, cpi, c_int, c_l2, c_line) / (MHZ * 1e3)


def ppa_pixels(log):
    """What the PPA turned in each measured redraw, as p4bench reports it."""
    return {int(m.group(1)): int(m.group(2)) for m in re.finditer(r"^ppa (\d+): (\d+) pixels", log, re.M)}


def report(p, ppa_px):
    cases = [(1.0, 10, 20, sum(COPY_MS) / 2, "middle"),
             (1.0, 10, 20, COPY_MS[0], "PSRAM faster"), (1.0, 10, 20, COPY_MS[1], "PSRAM slower"),
             (1.5, 20, 40, sum(COPY_MS) / 2, "slower core")]
    k = p[10]
    print(f"Calibration: memcpy 1280x620 RGB565 PSRAM to PSRAM, {k['insns']} instructions, "
          f"{k['psram_fill']} lines read and {k['psram_writeback']} written")
    for cpi, ci, c2, cm, what in cases:
        print(f"  {what:13s}: CPI {cpi}, internal line {ci}, L2 line {c2} cycles, copy {cm:.1f} ms "
              f"-> a PSRAM line costs {model(p, cpi, ci, c2, cm):.0f} cycles")
    print(f"DMA, measured on a Waveshare 7B: PPA turn {PPA_NS_PX:.1f} ns a pixel, display copy "
          f"{DISPLAY_NS_PX:.1f} ns a pixel\n")

    for name, px, ui, pic in BOARDS:
        for what, ph in (("the whole UI redrawn", ui), ("a full-screen picture", pic)):
            up, nat = p[ph], p[ph + 1]
            print(f"{name}, {what}, ms per frame on a P4:")
            print(f"  {'':40s}" + "".join(f"{c[4]:>14s}" for c in cases))
            rows = []
            for c in cases:
                cl = model(p, *c[:3], c[3])
                cu, co = ms(up, *c[:3], cl), ms(nat, *c[:3], cl)
                ppa, disp = px * PPA_NS_PX / 1e6, px * DISPLAY_NS_PX / 1e6
                rows.append([cu + ppa + disp, cu + ppa, co + disp, co])
            names = ["ESPHome lvgl / esp_lvgl_port", "esp_lvgl_adapter", "lv_draw_ortho (ESPHome's mipi_dsi)",
                     "lv_draw_ortho (panel frame buffers)"]
            for i, n in enumerate(names):
                print(f"  {n:40s}" + "".join(f"{r[i]:14.1f}" for r in rows))
            print(f"  {'lv_draw_ortho against the adapter':40s}" +
                  "".join(f"{r[1] / r[2]:13.2f}x" for r in rows))
            if ph + 2 in p and ph + 2 in ppa_px:
                # lv_draw_ortho with the PPA turning: the CPU's part counted,
                # the PPA's timed at the measured rate. The two overlap when a
                # tile is drawn while the last one is turned; between fully
                # overlapped and one after the other is where a board lands.
                k2, dma = p[ph + 2], ppa_px[ph + 2] * PPA_NS_PX / 1e6
                both, best, worst = [], [], []
                for c, r in zip(cases, rows):
                    cpu = ms(k2, *c[:3], model(p, *c[:3], c[3]))
                    lo, hi = max(cpu, dma), cpu + dma
                    both.append((lo, hi))
                    best.append(r[1] / lo)
                    worst.append(r[1] / hi)
                disp = px * DISPLAY_NS_PX / 1e6
                print(f"  {'lv_draw_ortho + PPA (ESPHome mipi_dsi)':40s}" +
                      "".join(f"{lo + disp:7.1f}-{hi + disp:<6.1f}" for lo, hi in both))
                print(f"  {'lv_draw_ortho + PPA (panel buffers)':40s}" +
                      "".join(f"{lo:7.1f}-{hi:<6.1f}" for lo, hi in both))
                print(f"  {'  against the adapter':40s}" +
                      "".join(f"{w:6.2f}-{b:.2f}x " for w, b in zip(worst, best)))
                print(f"    of which the PPA {dma:.1f} ms ({ppa_px[ph + 2]} pixels), the CPU "
                      f"{k2['insns'] / 1e6:.1f} M instructions and "
                      f"{(k2['psram_fill'] + k2['psram_writeback']) * 64 / 1024:.0f} KiB through the PSRAM")
            if ph == pic:
                # A camera through lvgl_camera_display's ortho_id: the PPA turns
                # each frame once into a window LVGL keeps out of, and LVGL
                # draws nothing there. The others draw it as the picture above.
                win = px * PPA_NS_PX / 1e6
                print(f"  {'camera through ortho_id (PPA window)':40s}" +
                      "".join(f"{win + px * DISPLAY_NS_PX / 1e6:14.1f}" for r in rows) +
                      "   (ESPHome's mipi_dsi)")
                print(f"  {'':40s}" + "".join(f"{win:14.1f}" for r in rows) + "   (panel frame buffers)")
            print(f"  CPU part: upright {up['insns'] / 1e6:.1f} M instructions, "
                  f"{(up['psram_fill'] + up['psram_writeback']) * 64 / 1024:.0f} KiB through the PSRAM; "
                  f"lv_draw_ortho {nat['insns'] / 1e6:.1f} M, "
                  f"{(nat['psram_fill'] + nat['psram_writeback']) * 64 / 1024:.0f} KiB\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--lvgl", help="an LVGL tree (v9.5.0), as for test/Makefile")
    ap.add_argument("--work", default=os.path.join(TEST, "build", "p4sim"))
    ap.add_argument("--counts", help="report from a counts file of an earlier run")
    a = ap.parse_args()
    if a.counts:
        logf = os.path.join(os.path.dirname(a.counts), "log.txt")
        report(read(a.counts), ppa_pixels(open(logf).read()) if os.path.exists(logf) else {})
        return
    if not a.lvgl:
        ap.error("--lvgl is needed to build")
    elf, so = build(a.lvgl, a.work)
    out = os.path.join(a.work, "counts.txt")
    log = simulate(elf, so, out)
    with open(os.path.join(a.work, "log.txt"), "w") as f:
        f.write(log)
    print(log, end="")
    # What the board does, or the figures say nothing about it: the tile in
    # internal RAM, and the run to its end.
    if "done" not in log or log.count("tile_internal 1") != 3:
        sys.exit("the simulated bench did not run as on a board (see above)")
    p = read(out)
    report(p, ppa_pixels(log))
    line = model(p, 1.0, 10, 20, sum(COPY_MS) / 2)
    if not 30 <= line <= 600:
        sys.exit(f"a PSRAM line comes out at {line:.0f} cycles: the calibration is off")


if __name__ == "__main__":
    main()
