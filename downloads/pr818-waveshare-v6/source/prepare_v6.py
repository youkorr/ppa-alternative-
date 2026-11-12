#!/usr/bin/env python3
"""Reconstruct the dedicated Waveshare 7B V6 application from pinned sources."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
from types import SimpleNamespace

import prepare
import install_fixture

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--fixtures', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    if args.out.exists() and any(args.out.iterdir()):
        raise ValueError('Use an empty output directory for this reconstruction')
    prepare.prepare(SimpleNamespace(repo=args.repo, fixtures=args.fixtures,
                                   out=args.out, board='waveshare-7b', revision='after_v2'))
    patch = HERE / 'candidate-v6.patch'
    subprocess.run(['git', 'apply', '--check', str(patch)], cwd=args.out, check=True)
    subprocess.run(['git', 'apply', str(patch)], cwd=args.out, check=True)
    app = args.out / prepare.ADAPTER / 'test_apps/lvgl9'
    reference_dir = args.out / 'reference-oracle'
    reference_dir.mkdir(exist_ok=True)
    references = json.loads((HERE / 'oracle-base64.json').read_text())
    expected = {name + '-320x480-16-r0-direct.bin.gz' for name in install_fixture.NAMES}
    if set(references) != expected:
        raise ValueError('The 16 frozen software reference frames are required')
    for name, data in references.items():
        (reference_dir / name).write_bytes(base64.b64decode(data, validate=True))
    install_fixture.install(app, reference_dir, '9.6.0')
    with (app / 'sdkconfig.bench').open('a') as output:
        output.write('\nCONFIG_LV_FONT_MONTSERRAT_14=y\nCONFIG_LV_FONT_MONTSERRAT_20=y\n')
    lock = (HERE / 'waveshare-7b-dependencies.lock').read_text()
    lock = lock.replace('/workspace/work/board-test-kit/stage', str(args.out.resolve()))
    (app / 'dependencies.lock').write_text(lock)
    meta = json.loads((args.out / 'benchmark.json').read_text())
    meta.update(revision='transforms_v6', reference_lvgl='9.6.0', cycles=2,
                scenes_per_rotation=16, expected_scene_logs=128,
                software_object_transforms='per-display explicit opt-in',
                automatic_hardware_pixel_equality=False,
                hardware_validation='pending',
                candidate_patch_sha256=hashlib.sha256(patch.read_bytes()).hexdigest())
    (args.out / 'benchmark.json').write_text(json.dumps(meta, indent=2) + '\n')
    print(app)


if __name__ == '__main__':
    main()
