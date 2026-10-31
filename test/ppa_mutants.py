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
    "a tile drawn before it was read back": ("        wait_ticket(&t);   /* it is drawn on next */\n", ""),
    # The layer handed back to LVGL with turns still running.
    "a run of tasks left running": ("    fence();\n    u->ticket[0] = u->ticket[1] = 0;\n",
                                     "    u->ticket[0] = u->ticket[1] = 0;\n"),
    # A camera frame handed back to the application mid-turn.
    "a picture given back mid-turn": (
        "    fence();   /* the picture is the application's again when the task is finished */\n", ""),
    # The PPA's angle the wrong way round.
    "the PPA turning the other way": (
        "        case LV_DISPLAY_ROTATION_90:\n            return PPA_SRM_ROTATION_ANGLE_90;",
        "        case LV_DISPLAY_ROTATION_90:\n            return PPA_SRM_ROTATION_ANGLE_270;"),
}
# NOT here: the fence before the CPU turns what the PPA declined. The two never
# write the same pixel; what they can share is a cache line at the edge of
# their rectangles, and the fake PPA has no cache to share it in. It stays in
# lv_draw_ortho for the board, as lv_draw_ortho_video.c's write-back does.


def main():
    build = HERE / (sys.argv[1] if len(sys.argv) > 1 else "build")
    lvgl = build / "lvgl.a"
    src = build / "lvgl-src"
    if not lvgl.exists():
        sys.exit(f"{lvgl} is missing: run make first")
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name, (old, new) in MUTANTS.items():
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
            cmd = ["cc", "-O2", "-DLV_CONF_INCLUDE_SIMPLE", f"-I{HERE}", f"-I{copy}", f"-I{src}", f"-I{HERE}/p4stub",
                   "-w", str(HERE / "main.c"), str(copy / "lv_draw_ortho.c"), str(copy / "lv_draw_ortho_video.c"),
                   str(copy / "lv_draw_ortho_ppa.c"), str(HERE / "p4stub" / "p4fake.c"), str(lvgl), "-lm", "-o", str(exe)]
            subprocess.run(cmd, check=True)
            r = subprocess.run([str(exe), "--ppa"], capture_output=True, text=True, timeout=600)
            caught = r.returncode != 0
            fails = sum(1 for l in r.stdout.splitlines() if "FAIL" in l)
            # A queued turn reading a picture already freed is a crash here,
            # as it would be memory read by DMA after its owner let it go.
            how = f"{fails} failing case(s)" if r.returncode > 0 else f"crashed, signal {-r.returncode}"
            print(f"  {'ok   ' if caught else 'MISSED'} caught: {name} ({how})")
            failed += not caught
    print(f"{len(MUTANTS) - failed} of {len(MUTANTS)} mutants caught")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
