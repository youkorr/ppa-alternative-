<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 youkorr
Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-

A follow-up comment for esp-iot-solution PR #818: it settles the earlier
comment that kept the PR a draft until the adapter integration had run on a
board. Everything below this block is what goes into the PR.
-->

Follow-up to my calibration comments above: the adapter integration has now run on both boards, which is why this PR is no longer a draft.

`test_apps/lvgl9`, built twice, the same but for `CONFIG_ESP_LVGL_ADAPTER_ROTATE_IN_DRAW`, all tests passing in every run (details in the description):

| frames/s | the adapter today (TRIPLE_PARTIAL) | this PR |
|---|---|---|
| Waveshare 7B, 90 | 15.8 | **17.2 - 17.3** |
| Waveshare 7B, 180 | 16.6 | **18.0** |
| M5Stack Tab5, 270 | 11.3 | **12.9 - 13.0** |
| M5Stack Tab5, 270, 128 KB L2 cache (ESP-IDF's default) | 11.2 | **17.9 - 18.0** |

Two corrections to what I wrote above:

- The PR no longer runs on one core with two frame buffers. It keeps the three frame buffers a rotated display already has: with two, every frame waited for the panel's refresh (66.1 ms instead of 58.4 at 90). And it paints the tiles on both cores.
- 61.1 ms on the Tab5 was not the PPA's floor. Through the adapter with ESP-IDF's default 128 KB L2 cache, a frame takes 53.3 - 54.4 ms. The quarter turn runs at 15-18 Mpx/s depending on the tile size, and the tiles come out of internal RAM, which this test app's 256 KB L2 cache cuts from 384 KiB to 256 KiB. The TRIPLE_PARTIAL path does not move with the cache size.
