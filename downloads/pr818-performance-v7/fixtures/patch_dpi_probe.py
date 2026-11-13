#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 youkorr
# SPDX-License-Identifier: MIT
"""Create a private, instrumented esp_lcd component for this benchmark only.

The SDK and its component cache are never edited. The probe observes the exact
framebuffer index selected for the next DMA scanout in IDF 5.5.6. All A/B builds
must use this identical instrumentation and the matching main probe module.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

MARKER = 'ab-benchmark: observe selected scanout generation'
INCLUDE = '''/* ab-benchmark: observe selected scanout generation */
extern void ab_scanout_latched_from_isr(esp_lcd_panel_handle_t panel, unsigned index) __attribute__((weak));
'''
ANCHOR = '''    uint8_t fb_index = dpi_panel->cur_fb_index;
    dw_gdma_link_list_handle_t link_list = dpi_panel->link_lists[fb_index];'''
DMA_START = '''    dw_gdma_channel_use_link_list(chan, link_list);
    dw_gdma_channel_enable_ctrl(chan, true);'''
HOOK = '''    /* A new DMA link list is selected below; repeated scanouts are filtered
     * by application generation in the benchmark's IRAM observer. */
    if (ab_scanout_latched_from_isr) ab_scanout_latched_from_isr(&dpi_panel->base, fb_index);'''


def prepare(idf, out):
    if (idf / 'version.txt').read_text().strip() != 'v5.5.6':
        raise ValueError('This probe is audited only for ESP-IDF v5.5.6')
    source = idf / 'components/esp_lcd'
    if out.exists():
        raise ValueError('Use an empty, private esp_lcd component output path')
    shutil.copytree(source, out, copy_function=shutil.copy2)
    original = source / 'dsi/esp_lcd_panel_dpi.c'
    data = original.read_text()
    assert MARKER not in data and data.count(ANCHOR) == 1 and data.count(DMA_START) == 1
    at = data.index('bool mipi_dsi_dma_trans_done_cb(')
    result = data[:at] + INCLUDE + '\n' + data[at:]
    result = result.replace(DMA_START, DMA_START + '\n' + HOOK, 1)
    (out / 'dsi/esp_lcd_panel_dpi.c').write_text(result)
    metadata = {'esp_idf': '5.5.6', 'test_only': True, 'changes_rendering_algorithm': False,
                'source_sha256': hashlib.sha256(original.read_bytes()).hexdigest(),
                'instrumented_sha256': hashlib.sha256(result.encode()).hexdigest(),
                'metric': 'distinct application generations selected for DMA scanout, not VSYNC count or optical glass measurement'}
    (out / 'ab-probe-manifest.json').write_text(json.dumps(metadata, indent=2) + '\n')
    return metadata


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--idf', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True, help='Normally APP/components/esp_lcd')
    args = parser.parse_args()
    print(json.dumps(prepare(args.idf, args.out), indent=2))
