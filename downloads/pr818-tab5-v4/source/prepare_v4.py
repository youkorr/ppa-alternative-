# SPDX-FileCopyrightText: 2026 youkorr
# SPDX-License-Identifier: MIT
"""Prepare bounded cache creation on the Tab5 with the stock IDF driver."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import prepare_v3


def make(args):
    app = prepare_v3.make(args)
    patch = Path(__file__).resolve().parent / 'cache-build-bands.patch'
    subprocess.run(['git', 'apply', '--check', str(patch)], cwd=args.out, check=True)
    subprocess.run(['git', 'apply', str(patch)], cwd=args.out, check=True)
    prepare_v3.replace_once(app / 'main/board_bench_config.h', '"after_v3"', '"after_v4"')
    prepare_v3.replace_once(app / 'main/board_bench.inc.c',
                           '    ESP_LOGI(TAG, "PR818_PPA_WORKAROUND mode=%s", BOARD_PPA_WORKAROUND);',
                           '    ESP_LOGI(TAG, "PR818_CACHE_PREPARATION max_bytes=131072 watchdog_ms=250");\n'
                           '    ESP_LOGI(TAG, "PR818_PPA_WORKAROUND mode=%s", BOARD_PPA_WORKAROUND);')
    path = args.out / 'benchmark.json'
    metadata = json.loads(path.read_text())
    metadata.update(revision='after_v4', rendering_patch='source/cache-build-bands.patch',
                    rendering_patch_sha256=hashlib.sha256(patch.read_bytes()).hexdigest(),
                    cache_prepare_max_bytes=131072, ppa_no_progress_timeout_ms=250,
                    hardware_validation='pending')
    path.write_text(json.dumps(metadata, indent=2) + '\n')
    return app


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--fixtures', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--idf', type=Path, required=True)
    parser.add_argument('--ppa-workaround', choices=['stock', 'espressif_bypass'], default='stock')
    print(make(parser.parse_args()))
