<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 youkorr
Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-

The text to paste into the pull request on espressif/esp-iot-solution.
Fill in the [brackets] once the boards have been tested.
-->

**Title:** feat(esp_lvgl_adapter): draw rotated DOUBLE_DIRECT displays in the panel's orientation

## Description

On RGB and MIPI DSI panels, a rotated display is drawn upright by LVGL and then
copied rotated into the frame buffer (`flush_dirty_copy`, `rotate_copy_region`),
with a third frame buffer for it. LVGL 9 can instead hand every draw task a
rotation matrix (`lv_display_set_matrix_rotation`), but its software renderer
ignores that matrix, so the option draws nothing on an ESP32.

This adds `CONFIG_ESP_LVGL_ADAPTER_ROTATE_IN_DRAW` (LVGL v9, **off by default**).
With rotation 90/180/270 and `ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_DIRECT`, the
display is set up exactly like an unrotated `DOUBLE_DIRECT` one — LVGL draws into
the panel's two frame buffers and the flush only switches them — and LVGL is
given the rotation with matrix rotation on. A draw unit, `lv_draw_ortho`, reads
the matrix:

- widgets are drawn by LVGL's own software renderer into a 128 KiB upright tile
  in internal RAM, so the pixels are LVGL's;
- on chips with a PPA, the PPA rotates each tile into the frame buffer while the
  CPU draws the next one, and fills plain opaque rectangles;
- an opaque unscaled image is rotated by the PPA in one pass;
- LVGL itself keeps the two buffers in sync in the panel's orientation
  (`refr_sync_areas` rotates the sync areas under matrix rotation).

What changes for a user who enables it:

- **2 frame buffers instead of 3** for rotated `DOUBLE_DIRECT`
  (`esp_lv_adapter_get_required_frame_buffer_count` returns 2);
- nothing is drawn upright and copied rotated afterwards;
- dummy-draw frames are shown as the panel stores them (not rotated);
- every other mode, rotation 0, mono and LVGL v8 are unchanged.

The PPA's known faults are handled: every PPA wait is bounded (250 ms) and a PPA
that stops answering falls back to the CPU; blocks the ESP-IDF v5.5 driver would
hang on at 90/270 (DIG-734, esp-idf #19023 / #19096) are split before they are
sent, using the driver's own test both ways.

`lv_draw_ortho` comes from https://github.com/youkorr/ppa-alternative-
(MIT, same author), where it is checked pixel for pixel against LVGL 9.5, 9.6
and master on a host build: 90/180/270, RGB565 and XRGB8888, direct and
partial modes, against a stand-in PPA that reproduces DIG-734 and holds every
transfer until it is waited for.

## Related

- LVGL matrix rotation: `lv_display_set_matrix_rotation()`
- esp-idf #19023, #19096 (DIG-734)
- lvgl/lvgl#10265 (PPA unit, scaled and rotated images)

## Testing

- Host: the draw unit's test suite (pixel-exact against LVGL), see the repository above.
- Build: `test_apps/lvgl9` for esp32p4 with `sdkconfig.ci.rotate_in_draw`, ESP-IDF v5.5.5.
- Hardware: [M5Stack Tab5, 720x1280, ROTATE_270: ... ms per frame before / after]
- Hardware: [Waveshare ESP32-P4 7B, 1024x600, ROTATE_180: ... ms per frame before / after]
- Test app: [`[adapter]` tests on the P4 with `sdkconfig.ci.rotate_in_draw`: pass / fail]

---

## Checklist

- [x] Off by default, other modes unchanged
- [x] CHANGELOG and version (0.8.0)
- [x] README and README_CN
- [x] Formatted with the repository's astyle rules, codespell clean
- [ ] Tested on hardware
- [ ] Contributor agreement signed (asked for when the PR is opened)
