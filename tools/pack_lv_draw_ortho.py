#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 youkorr
# Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
"""Stage lv_draw_ortho as an ESP Component Registry component.

registry/lv_draw_ortho holds the manifest, the README, the example and the
CMake file; the engine's sources stay where ESPHome's lvgl_ortho uses them
(components/lvgl_ortho), one copy, and are put in the staged component's
src/ here. The CMake file builds from src/ when it is there.

    python3 tools/pack_lv_draw_ortho.py OUT_DIR

Then `compote component pack --name lv_draw_ortho --project-dir OUT_DIR`, or
`compote component upload ...` with IDF_COMPONENT_API_TOKEN set.
"""
import pathlib
import shutil
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
PACKAGE = ROOT / "registry" / "lv_draw_ortho"
ENGINE = ROOT / "components" / "lvgl_ortho"
SOURCES = ["lv_draw_ortho.c", "lv_draw_ortho.h", "lv_draw_ortho_ppa.c", "lv_draw_ortho_ppa.h",
           "lv_draw_ortho_video.c", "lv_draw_ortho_video.h"]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = pathlib.Path(sys.argv[1]).resolve()
    if out.exists() and any(out.iterdir()):
        sys.exit(f"{out} must be new or empty")
    shutil.copytree(PACKAGE, out, dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns("build", "managed_components", "sdkconfig", "sdkconfig.old",
                                                  "dependencies.lock", "__pycache__"))
    (out / "src").mkdir()
    for name in SOURCES:
        shutil.copy2(ENGINE / name, out / "src" / name)
    # Every engine file the ESPHome component has, bar its own glue, must be
    # in the package: a new one forgotten here would build in the repository
    # and not from the registry.
    engine = {p.name for p in ENGINE.glob("lv_draw_ortho*.[ch]")}
    missing = engine - set(SOURCES)
    if missing:
        sys.exit(f"engine files not packed: {sorted(missing)}")
    print(out)


if __name__ == "__main__":
    main()
