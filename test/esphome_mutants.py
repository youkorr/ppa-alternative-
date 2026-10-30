#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 youkorr
# Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
"""The ESPHome component's test, checked against broken copies of the component.

Each mutant is lvgl_ortho.cpp with one rule taken out; esphome_test must
fail on every one, or it is not testing that rule. Run after `make run`:

    python3 esphome_mutants.py [BUILD]      (default: build)
"""

import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ORTHO = HERE.parent / "components" / "lvgl_ortho"
MUTANTS = {
    "touch not turned": (
        "      lv_display_rotate_point(g_self->disp_, &data->point);",
        "      ;",
    ),
    "flush area not turned": (
        "    lv_draw_ortho_native_area(disp, area, &native);",
        "    native = *area;",
    ),
    "narrow area packed from x 0": ("+ a.x1 * 2,", "+ 0,"),
    "ESPHome's 90 taken as LVGL's 90": (
        "    case 90:\n      return LV_DISPLAY_ROTATION_270;",
        "    case 90:\n      return LV_DISPLAY_ROTATION_90;",
    ),
    "frame not centred": (
        "const int32_t fx = at.x1 + (ow - width) / 2, fy = at.y1 + (oh - height) / 2;",
        "const int32_t fx = at.x1, fy = at.y1;",
    ),
}


def main():
    build = HERE / (sys.argv[1] if len(sys.argv) > 1 else "build")
    objs = sorted(str(p) for p in (build / "esphome").glob("*.o"))
    if not objs:
        sys.exit(f"no objects in {build}/esphome: run `make run` first")
    text = (ORTHO / "lvgl_ortho.cpp").read_text()
    missed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name, (old, new) in MUTANTS.items():
            assert text.count(old) == 1, f"mutant '{name}' no longer applies"
            copy = pathlib.Path(tmp) / "c"
            shutil.rmtree(copy, ignore_errors=True)
            shutil.copytree(ORTHO, copy)
            (copy / "lvgl_ortho.cpp").write_text(text.replace(old, new))
            exe = pathlib.Path(tmp) / "t"
            cmd = ["g++", "-O1", "-std=c++17", "-DLV_CONF_INCLUDE_SIMPLE", f"-I{HERE}", f"-I{copy}",
                   f"-I{build}/lvgl-src", f"-I{HERE}/esphome_stub", f"-I{HERE}/p4stub",
                   str(HERE / "esphome_test.cpp"), str(copy / "lvgl_ortho.cpp"), *objs,
                   str(build / "lvgl.a"), "-lm", "-o", str(exe)]
            subprocess.run(cmd, check=True, cwd=HERE)
            caught = any(subprocess.run([str(exe), r], capture_output=True).returncode for r in ("90", "180", "270"))
            print(("  ok    caught: " if caught else "  FAIL  missed: ") + name)
            missed += not caught
    sys.exit(1 if missed else 0)


if __name__ == "__main__":
    main()
