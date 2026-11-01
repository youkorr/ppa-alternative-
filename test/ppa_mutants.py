#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 youkorr
# Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
"""The PPA tests, checked against copies of lv_draw_ortho with a wait taken out.

The fake PPA runs a queued transaction only when the caller waits, so a wait
lv_draw_ortho forgets leaves the frame buffer -- or a tile -- as it was, and a
picture comes out wrong. Each mutant below takes one wait out; `ortho_test
--ppa` must fail on every one, or it is not testing that wait. Run after
`make`:

    python3 ppa_mutants.py LVGL_BUILD_DIR      (default: build)

With `--cores BUILD_DIR` (an LVGL built with LV_USE_OS=1, see test/Makefile),
it checks the second core instead: `ortho_test --cores` must fail on each of
CORES_MUTANTS.
"""

import pathlib
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ORTHO = HERE.parent / "components" / "lvgl_ortho"
MUTANTS = {
    # A tile drawn on while the PPA is still turning it out.
    "a tile reused before its turn finished": ("            wait_ticket(&u->ticket[k]);\n", ""),
    # A tile drawn on before the PPA has read the frame buffer back into it.
    "a tile drawn before it was read back": ("        if(!wait_ticket(&t))   /* it is drawn on next */\n",
                                             "        if(0)\n"),
    # The layer handed back to LVGL with turns still running.
    "a run of tasks left running": ("    fence();\n    for(int i = 0; i < 4; i++) u->ticket[i] = 0;\n",
                                     "    for(int i = 0; i < 4; i++) u->ticket[i] = 0;\n"),
    # A camera frame handed back to the application mid-turn.
    "a picture given back mid-turn": (
        "    fence();   /* the picture is the application's again when the task is finished */\n", ""),
    # DIG-734: a block the P4 never finishes sent whole (esp-idf #19023).
    "DIG-734 blocks sent whole": (
        "    if(!cut(0, 0, bw, bh, rot, px * 8, pieces, &n, 0)) return 0;\n",
        "    pieces[n++] = (piece_t) {0, 0, bw, bh};\n"),
    # A wait with no bound: a stuck PPA freezes the screen (the driver's own
    # blocking mode). The test would hang, so the run has a timeout.
    "a wait with no bound": ("        else if((idle_us += 10) >= LV_DRAW_ORTHO_PPA_TIMEOUT_MS * 1000u) {",
                             "        else if(0) {"),
    # A stalled PPA not given up on for good: the video tries it again, and
    # waits out the timeout every frame.
    "a stuck PPA used again": ("    if(g_stuck) return ESP_ERR_INVALID_STATE;\n    esp_err_t err = ensure_client();",
                               "    esp_err_t err = ensure_client();"),
    # A fill given the colour at 8 bits a channel: a PPA that rounds when it
    # narrows to RGB565 would put a colour one step off LVGL's. The fake
    # truncates, so the mutant is the fill colour narrowed the other way.
    "a fill colour off by a step": ("    const uint32_t r = cf == LV_COLOR_FORMAT_RGB565 ? (color.red & 0xF8u) : color.red;",
                                    "    const uint32_t r = cf == LV_COLOR_FORMAT_RGB565 ? ((color.red + 4u > 255u ? 255u : color.red + 4u) & 0xF8u) : color.red;"),
    # A fill left undrawn because some later picture exists, covering it or not.
    "a fill skipped that a picture does not cover": ("        if(lv_area_is_in(todo, &covered, 0)) return true;",
                                                     "        return true;"),
    # The PPA's angle the wrong way round.
    "the PPA turning the other way": (
        "        case LV_DISPLAY_ROTATION_90:\n            return PPA_SRM_ROTATION_ANGLE_90;",
        "        case LV_DISPLAY_ROTATION_90:\n            return PPA_SRM_ROTATION_ANGLE_270;"),
}
# The second core: each must make `ortho_test --cores` fail. The third item
# is extra compiler flags: LV_DRAW_ORTHO_TEST_TILES=2 leaves no room for more
# than two tiles, so the four are halves of those.
CORES_MUTANTS = {
    # The other core's tile handed to it while the PPA still turns it out.
    "the other core's tile reused mid-turn": ("    wait_ticket(&u->ticket[base + 1]);\n", "", []),
    # The other core's tile turned out before it has finished painting it.
    "turned out before the other core finished": (
        "    if(ok[1]) {\n        helper_join();\n        g_stats.tiles_other_core++;\n    }\n"
        "    if(ok[0]) finish_tile(o, &w[0], &u->ticket[base]);\n    if(ok[1]) finish_tile(o, &w[1], &u->ticket[base + 1]);\n",
        "    if(ok[0]) finish_tile(o, &w[0], &u->ticket[base]);\n    if(ok[1]) finish_tile(o, &w[1], &u->ticket[base + 1]);\n"
        "    if(ok[1]) {\n        helper_join();\n        g_stats.tiles_other_core++;\n    }\n", []),
    # Two of the four halves in the same memory: the cores paint over each other.
    "two halves overlapping": ("            u->tile[3] = u->tile[1] + half;\n", "            u->tile[3] = u->tile[0] + half;\n",
                               ["-DLV_DRAW_ORTHO_TEST_TILES=2"]),
    # The pair's first tile drawn into the other core's tile.
    "both cores in one tile": (
        "    ok[0] = prep_tile(jobs, n, o, &rect[0], u->tile[base], u->tile_bytes, &w[0]);",
        "    ok[0] = prep_tile(jobs, n, o, &rect[0], u->tile[base + 1], u->tile_bytes, &w[0]);", []),
}

# NOT here: the fence before the CPU turns what the PPA declined. The two never
# write the same pixel; what they can share is a cache line at the edge of
# their rectangles, and the fake PPA has no cache to share it in. It stays in
# lv_draw_ortho for the board, as lv_draw_ortho_video.c's write-back does.


def main():
    args = sys.argv[1:]
    cores = bool(args) and args[0] == "--cores"
    if cores:
        args = args[1:]
    mutants = CORES_MUTANTS if cores else {k: (o, n, []) for k, (o, n) in MUTANTS.items()}
    build = HERE / (args[0] if args else ("build-mt" if cores else "build"))
    lvgl = build / "lvgl.a"
    src = build / "lvgl-src"
    if not lvgl.exists():
        sys.exit(f"{lvgl} is missing: run make first")
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name, (old, new, flags) in mutants.items():
            copy = pathlib.Path(tmp) / "c"
            shutil.rmtree(copy, ignore_errors=True)
            shutil.copytree(ORTHO, copy)
            hit = 0
            for f in ("lv_draw_ortho.c", "lv_draw_ortho_ppa.c"):
                p = copy / f
                text = p.read_text()
                hit += text.count(old)
                p.write_text(text.replace(old, new))
            assert hit == 1, f"mutant '{name}' no longer applies ({hit} matches)"
            exe = pathlib.Path(tmp) / "t"
            os_flags = ["-DLV_USE_OS=1"] if cores else []
            cmd = ["cc", "-O2", "-DLV_CONF_INCLUDE_SIMPLE", *os_flags, *flags, f"-I{HERE}", f"-I{copy}", f"-I{src}",
                   f"-I{HERE}/p4stub", "-w", str(HERE / "main.c"), str(copy / "lv_draw_ortho.c"),
                   str(copy / "lv_draw_ortho_video.c"), str(copy / "lv_draw_ortho_ppa.c"),
                   str(HERE / "p4stub" / "p4fake.c"), str(lvgl), "-lm", "-lpthread", "-o", str(exe)]
            subprocess.run(cmd, check=True)
            try:
                r = subprocess.run([str(exe), "--cores" if cores else "--ppa"], capture_output=True, text=True,
                                   timeout=300)
            except subprocess.TimeoutExpired:
                print(f"  ok    caught: {name} (hung, as a board would)")
                continue
            caught = r.returncode != 0
            fails = sum(1 for l in r.stdout.splitlines() if "FAIL" in l)
            # A queued turn reading a picture already freed is a crash here,
            # as it would be memory read by DMA after its owner let it go.
            how = (f"{fails} failing case(s)" if r.returncode > 0 else
                   f"crashed, signal {-r.returncode}" if r.returncode < 0 else "every case passed")
            print(f"  {'ok   ' if caught else 'MISSED'} caught: {name} ({how})")
            failed += not caught
    print(f"{len(mutants) - failed} of {len(mutants)} mutants caught")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
