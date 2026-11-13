#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 youkorr
# SPDX-License-Identifier: MIT
"""Instrument only a private benchmark copy of the native PPA wait function."""
import argparse
import hashlib
import json
from pathlib import Path

MARKER = 'ab-cost-v8: private native wait observer'
SIGNATURE = 'static bool wait_on(volatile uint32_t * done, uint32_t ticket)\n{'
NEXT = '/* As in lv_draw_ortho_video.c: LVGL\'s 90 and the PPA\'s 90 are the same turn. */'
DECLARATIONS = '''/* ab-cost-v8: private native wait observer */
#include "esp_timer.h"
extern uint32_t ab_cost_native_wait_begin(void) __attribute__((weak));
extern void ab_cost_native_wait_end(uint32_t token, uint32_t elapsed_us,
                                    bool pending, bool success) __attribute__((weak));
'''
WRAPPER = '''static bool wait_on(volatile uint32_t * done, uint32_t ticket)
{
    if (!ab_cost_native_wait_begin || !ab_cost_native_wait_end) {
        return wait_on_unprobed(done, ticket);
    }
    const uint32_t token = ab_cost_native_wait_begin();
    if (!token) return wait_on_unprobed(done, ticket);
    const bool pending = (int32_t)(*done - ticket) < 0;
    const int64_t started = esp_timer_get_time();
    const bool result = wait_on_unprobed(done, ticket);
    ab_cost_native_wait_end(token, (uint32_t)(esp_timer_get_time() - started), pending, result);
    return result;
}

'''


def patch(source):
    original = source.read_bytes()
    text = original.decode()
    if MARKER in text:
        raise ValueError('Private source already instrumented; begin from the pinned clean component')
    if text.count(SIGNATURE) != 1 or text.count(NEXT) != 1:
        raise ValueError('Unexpected native source; refusing an unaudited patch')
    text = text.replace(SIGNATURE, DECLARATIONS + '\n' + SIGNATURE.replace('wait_on(', 'wait_on_unprobed('), 1)
    text = text.replace(NEXT, WRAPPER + NEXT, 1)
    # Break any shared cache hardlink before writing a private source change.
    source.unlink()
    source.write_text(text)
    return {'test_only': True, 'algorithm_changed': False,
            'source_sha256': hashlib.sha256(original).hexdigest(),
            'instrumented_sha256': hashlib.sha256(text.encode()).hexdigest(),
            'metric': 'inclusive wall time for native wait calls; pending was true at entry, not proof of entire call blocked'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(patch(args.source), indent=2))
