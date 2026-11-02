<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 youkorr
Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-

The text to paste into the pull request on espressif/esp-iot-solution.
Hardware measurements go in "Testing" before the draft is marked ready.
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
sent, using the driver's own test both ways. That matters because the
driver's fix is on master and release/v6.0 but not on release/v6.1 or
release/v5.5 (checked 2026-11-01), so this needs neither the IDF patch shipped
beside this component nor a particular IDF version.

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
- Build: `test_apps/lvgl9` for esp32p4 with `sdkconfig.ci.rotate_in_draw` and `sdkconfig.ci.p4rev3`, ESP-IDF v5.5.5, release-v6.0, release-v6.1 and latest (the images this test app is built with).
- Hardware: **not yet run on a board** — opened as a draft for that reason.
  Measurements on an M5Stack Tab5 (720x1280, ROTATE_270) and a Waveshare
  ESP32-P4 7B (1024x600, ROTATE_180) will be added here before it is marked
  ready for review.
- Timing estimate: the same code built for the P4's RISC-V core and run in
  QEMU under a cache model of the P4, with the PPA and display timed at rates
  measured on these boards. A full redraw of a dashboard-like UI: 88.4 ms with
  today's rotated path, 44.8 ms with this option (34.7 ms with the draw unit's
  optional second core, not part of this PR); a full-screen image: 86.7 ms
  against 22.4 ms. These are simulated figures, to be confirmed on hardware.

---

## Checklist

Before submitting a Pull Request, please ensure the following:

- [x] 🚨 This PR does not introduce breaking changes. (The option is off by default; nothing changes unless it is enabled.)
- [ ] All CI checks (GH Actions) pass. (Builds with ESP-IDF v5.5.5, release-v6.0, release-v6.1 and latest in the author's CI; waiting for this repository's.)
- [x] Documentation is updated as needed. (README, README_CN, Kconfig help, CHANGELOG, version 0.8.0.)
- [x] Tests are updated or added as necessary. (lvgl9 test app runs on a rotated DOUBLE_DIRECT display with the option; `sdkconfig.ci.rotate_in_draw`.)
- [x] Code is well-commented, especially in complex areas.
- [x] Git history is clean — commits are squashed to the minimum necessary. (One commit.)
- [ ] Tested on hardware (draft until then).

🤖 Generated with [Claude Code](https://claude.com/claude-code)
