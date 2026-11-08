<!--
SPDX-License-Identifier: MIT
Copyright (c) 2026 youkorr
Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-

The description of esp-iot-solution PR #818 as it stands on GitHub
(https://github.com/espressif/esp-iot-solution/pull/818), kept here so it can
be edited with the code and pasted back.
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
display is set up like an unrotated `DOUBLE_DIRECT` one — LVGL draws straight
into the panel's frame buffers and the flush only switches them — and LVGL is
given the rotation with matrix rotation on. A draw unit, `lv_draw_ortho`, reads
the matrix:

- widgets are drawn by LVGL's own software renderer into a 128 KiB upright tile
  in internal RAM, so the pixels are LVGL's;
- on chips with a PPA, the PPA rotates each tile into the frame buffer while the
  CPU draws the next one, and fills plain opaque rectangles;
- an opaque unscaled image is rotated by the PPA in one pass;
- with `LV_DRAW_SW_DRAW_UNIT_CNT` above 1 on a dual-core chip, every other tile
  is painted on the second core; where internal RAM has room for a single
  128 KiB block (an ESP32-P4 whose L2 cache takes 256 KiB), the tiles are cut
  out of that block;
- the display keeps the **three** panel buffers a rotated display is given
  today: LVGL draws the next frame into the one neither on screen nor waiting
  to be, so a frame is not held until the panel's next refresh. LVGL brings
  that buffer up to date with the frame before (`refr_sync_areas`, which
  rotates the sync areas under matrix rotation); the frame before that is
  copied from the adapter's existing diff tracker at `LV_EVENT_RENDER_START`,
  less what is about to be drawn again.

What changes for a user who enables it:

- the same frame buffers as today: `esp_lv_adapter_get_required_frame_buffer_count`
  still returns 3 for a rotated display. A panel made with 2 still works, and
  each frame then waits for the panel's refresh (a warning says so);
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
- Target: `test_adapter_esp32p4_rotate_in_draw` in `pytest_adapter_lvgl9.py` runs the same feature tests with `sdkconfig.ci.rotate_in_draw` (display rotated 90 degrees, `DOUBLE_DIRECT`) on the `esp32p4_function_ev_board,screen,touch,eco_default` runner, the rev 3 board none of mine is.
- Rebased on v0.7.5. With v0.7.5's PPA blend acceleration, `profile.enable_ppa_accel` is ignored (with a warning) on a display rotated this way: `lv_draw_ortho` already uses the PPA there, and the blend handlers on the same tiles would put two PPA users under one refresh. Other displays keep their setting.

### Measured through this adapter

`test_apps/lvgl9` on a Waveshare ESP32-P4-WIFI6-Touch-LCD-7B (the EV board's
EK79007 1024x600 panel; ESP32-P4 rev 1.3; ESP-IDF v5.5.5), built twice, the
same but for this option. A dashboard-like UI (text, cards with borders and
shadows, a gradient, an arc, a bar, a slider, a switch) is redrawn whole for
10 s; frames are counted, and each is timed from `LV_EVENT_RENDER_START` to
`LV_EVENT_REFR_READY`. Two runs each:

| | frame buffers | 90 degrees | 180 degrees |
|---|---|---|---|
| the adapter today: `DEFAULT_MIPI_DSI` (TRIPLE_PARTIAL), drawn upright and rotated by the PPA | 3 | 63.4 ms | 59.4 - 59.7 ms (16.6/s) |
| this PR, `ROTATE_IN_DRAW`, two cores | 3 | **57.5 - 58.2 ms (17.2 - 17.3/s)** | **54.9 - 55.2 ms (18.0/s)** |

All eight tests pass in every configuration. So with the same three frame
buffers this PR is **8-9% faster at 90 degrees and 7-8% at 180** on a P4
below rev 3.0, where the adapter already overlaps drawing and turning in
TRIPLE_PARTIAL; what it saves besides is the rotated copy of each frame.

The same test app on an **M5Stack Tab5** (720x1280 ILI9881C, ESP32-P4 rev 1.0)
held in landscape, ROTATE_270, two runs each:

| | ESP-IDF | frames/s | ms per frame (worst) |
|---|---|---|---|
| the adapter today: `DEFAULT_MIPI_DSI` (TRIPLE_PARTIAL) | v5.5.5 | 11.3 | 81.5 (103) |
| this PR, `ROTATE_IN_DRAW`, two cores | v5.5.5 | **13.0** | **77.0 - 77.5 (78)** |
| the same | v6.1 | **13.0 - 13.2** | **75.8 - 76.9 (78)** |

15% more frames a second on the Tab5, with a steadier worst frame; ESP-IDF
v6.1 measures the same as v5.5.5. (frames/s is the figure to compare: the
adapter's TRIPLE_PARTIAL path loses about 7 ms between frames that its
per-frame time does not show.)

An earlier version of this PR used two frame buffers and was *slower* at 90
degrees, 66.1 ms: it drew a frame in 58.4 ms and then waited for the panel's
refresh to switch buffers, which on a 60 Hz panel rounded every frame up to
four refreshes. That is what the third buffer removes. Rev 3.0 silicon has not
been measured; the pytest case above is what runs it on one.

Running the test app also found two faults, both fixed in this PR:

- `lv_deinit()` deletes every draw unit, and the adapter created
  `lv_draw_ortho` only once: after the adapter was restarted, a display
  rotated in draw had no unit to draw it.
- `lv_draw_ortho` draws on the LVGL task rather than on LVGL's draw threads,
  so that task needs their stack: FreeType overflowed the default 8 KB. The
  task's stack is now raised to `LV_DRAW_THREAD_STACK_SIZE` with this option.
- The second core was refused on that board: with a 256 KiB L2 cache, internal
  RAM holds one 128 KiB block, and the second core wants tiles of its own. The
  tiles are now cut out of the blocks there are.

The build and the timing code are in
https://github.com/youkorr/ppa-alternative-/tree/main/espressif/waveshare-7b.

### Measured under ESPHome

The figures below compare `lv_draw_ortho` against ESPHome's own LVGL rotation,
which is slower than this adapter's TRIPLE_PARTIAL path; they are not a
comparison with the adapter.

Two ESP32-P4 boards below rev 3.0, ESP-IDF 5.5.5 and 5.5.6. These were
measured with the same draw unit, `lv_draw_ortho`, running under ESPHome's
LVGL (the `lvgl_ortho` component of the repository above), not yet through
this adapter. The same UI (text, cards with borders and shadows, a gradient,
an arc, a bar, a slider, a switch) is redrawn whole on every refresh.

**Waveshare ESP32-P4-WIFI6-Touch-LCD-7B**, 1024x600, ROTATE_180:

| | ms per frame |
|---|---|
| LVGL's own rotation (drawn upright, copied rotated) | 129.0 |
| `lv_draw_ortho`, 1 core, drawn into the panel's own frame buffers | **57.0** (2.3x) |
| the same, 2 cores | **47.7** (2.7x) |

**M5Stack Tab5**, 720x1280 used at ROTATE_270:

| | ms per frame |
|---|---|
| LVGL's own rotation (drawn upright, copied rotated) | 193.6 |
| the same, with an LVGL PPA draw unit | 176.1 |
| `lv_draw_ortho`, the CPU turning the tiles | 177 |
| `lv_draw_ortho`, the PPA turning, 1 core, frame copied to the panel | 78.5 |
| `lv_draw_ortho`, the PPA turning, 2 cores, frame copied to the panel | 71.0 |
| `lv_draw_ortho`, 2 cores, drawn into the panel's own frame buffers | **61.1** |

At 61 ms the Tab5's frame is the
PPA's 270 turn of 1280x720 and nothing else: on rev 1.0 the SRM works in 16x16 macro blocks
at 90/270 (32x32 from rev 3.0), about 15 Mpx/s on this board, and the PSRAM
the PPA, both cores and the DSI scanout share is the limit.

At 180 the turn is the PPA's easy case: the Waveshare is most likely
limited by drawing the UI rather than by turning it (per pixel it is slower
than the Tab5), though the log does not split the two.

A camera on the whole screen, each frame drawn by LVGL as a picture and
turned once by the PPA. On the Waveshare (OV5647 at 1280x960, the middle
1024x600 shown, ROTATE_180): **20.8 frames/s**. On the Tab5 (SC2356,
1280x720, ROTATE_270):

| | camera frames/s |
|---|---|
| LVGL's own rotation | 10-12 |
| `lv_draw_ortho`, frame copied to the panel | 12.8 |
| `lv_draw_ortho`, drawn into the panel's own frame buffers | **15.6** |

On the Tab5 the copy to the panel costs 14.4 ms a frame, which is what drawing
into the panel's own buffers saves -- the case this PR enables.

No PPA hang was seen in any of these runs (IDF 5.5.5 and
5.5.6, both without the driver fix): blocks that would hang are cut in
software before they are sent, with no IDF patch.

- Hardware: `test_apps/lvgl9` run on a Waveshare 7B at 90 and 180 degrees and on an M5Stack Tab5 at 270, with and without this option, ESP-IDF v5.5.5 (and v6.1 on the Tab5), all tests passing (above).

---

## Checklist

- [x] Off by default, other modes unchanged
- [x] CHANGELOG and version (0.8.0), rebased on v0.7.5
- [x] README and README_CN
- [x] Formatted with the repository's astyle rules, codespell clean
- [x] Draw unit measured on hardware (Waveshare 7B and M5Stack Tab5, through ESPHome)
- [x] Adapter integration run on hardware (Waveshare 7B at 90 and 180, M5Stack Tab5 at 270): all tests pass; 8-9% faster than TRIPLE_PARTIAL at 90, 7-8% at 180, 15% more frames/s on the Tab5, same frame buffers
- [ ] Contributor agreement signed (asked for when the PR is opened)
