# lv_draw_ortho

An LVGL 9 draw unit for the ESP32-P4 that draws a rotated display in the
panel's own orientation, so nothing has to turn the frame afterwards.

LVGL normally draws a rotated screen upright and the whole frame is then turned
onto the panel, by the CPU or by the PPA: every pixel is read and written again
each frame. lv_draw_ortho turns LVGL's matrix rotation into work the P4 does
well:

- **Tiles.** LVGL's software renderer draws a small upright tile in internal
  RAM, and the PPA turns it into the frame buffer while the next tile is drawn
  -- on both cores when there are two.
- **Plain rectangles** are filled by the PPA, straight into the frame buffer.
- **Big pictures** (a wallpaper, a camera frame, a decoded video) are turned
  by the PPA straight into the frame buffer in one pass. A picture in PSRAM goes
  through two bands of internal RAM, copied by the AXI GDMA, because the PPA
  reads PSRAM in 16x16 macro blocks at half speed. The bands cost no RAM: they
  are the halves of the first tile, lent while the picture is turned.
- **Pictures that do not change** can be turned once and kept in PSRAM.
- **LVGL's copies between frame buffers** in DIRECT mode are written down and
  made only where the next frame does not draw, by the PPA.

It is the engine of [ppa-alternative](https://github.com/youkorr/ppa-alternative-)
(ESPHome's `lvgl_ortho`), packed on its own. It was also proposed to
esp-iot-solution's `esp_lvgl_adapter` as
[PR 818](https://github.com/espressif/esp-iot-solution/pull/818), which
Espressif decided not to merge into the general-purpose adapter; this component
is the optional backend for the applications that want it. It is not an
Espressif component.

## Measured

Espressif's reviewers, on PR 818: two ESP32-P4 boards, 1024x600 MIPI DSI
RGB565, ESP-IDF 5.5.6, LVGL 9.6, 90 degrees, big pictures in PSRAM: from about
29.5 to 60.9 refreshes a second on one board and from 25.0 to 38.8 on the other,
the average CPU load across both cores from about 39 % to 2 %.

The same engine in esp_lvgl_adapter's lvgl9 test app, ESP-IDF 5.5.6, LVGL 9.6,
refreshes a second against the adapter as Espressif ships it (turning the
frame afterwards):

| | Waveshare 7B, 1024x600 at 180 | | M5Stack Tab5, 1280x720 at 270 | |
|---|---:|---:|---:|---:|
| | adapter | lv_draw_ortho | adapter | lv_draw_ortho |
| dashboard, partial updates | 61.3 | 60.3 | 48.4 | 48.2 |
| dashboard redrawn whole | 14.1 | 21.3 | 7.6 | 13.4 |
| picture in PSRAM changed every frame | 12.5 | 32.8 | 10.0 | 16.4 |
| camera (MIPI CSI through the ISP) | 12.4 | 29.2 | 10.0 | 16.3 |
| picture in the flash | 4.4 | 5.0 | 2.9 | 3.2 |

## Requirements

- ESP32-P4. Version 0.1.0 was tested on ESP-IDF 5.5.5 and 5.5.6.
  Version 0.2.0 targets ESP-IDF 5.5 through 6.1, with CI building the packaged
  Waveshare example on 5.5.6 and 6.1.1. Hardware results below are historical;
  a successful build is not an on-device validation of the new package.
- LVGL 9.5 or 9.6. The engine reads LVGL's private structures, so only the
  tested versions are allowed by the manifest.
- `CONFIG_LV_USE_FLOAT=y`, `CONFIG_LV_USE_MATRIX=y` and
  `CONFIG_LV_DRAW_TRANSFORM_USE_MATRIX=y`.
- A display in `LV_DISPLAY_RENDER_MODE_DIRECT` or `FULL`, of the panel's own
  size. (PARTIAL mode needs a small LVGL patch, kept in the repository's
  `patches/`.)
- `CONFIG_LV_OS_FREERTOS=y` for the second core.
- Internal RAM for the tiles: one 128 KiB block per tile, up to four (two with
  one core). With less room the tiles are cut smaller from the blocks there
  are; `LV_DRAW_ORTHO_TILE_BYTES` sets the block size.

## Use

```c
#include "lv_draw_ortho.h"
#include "lv_draw_ortho_ppa.h"

/* The display at the panel's own size, its buffers the panel's frame buffers. */
lv_display_t *disp = lv_display_create(PANEL_W, PANEL_H);
lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
lv_display_set_buffers(disp, fb0, fb1, PANEL_W * PANEL_H * 2, LV_DISPLAY_RENDER_MODE_DIRECT);
lv_display_set_flush_cb(disp, flush_cb);

lv_draw_ortho_init();                      /* the draw unit */
if (lv_draw_ortho_ppa_init() == ESP_OK) {  /* the PPA turns the tiles and fills */
    lv_draw_ortho_ppa_use_tiles(true);     /* big PSRAM pictures through the lent tile */
}
lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);
lv_display_set_matrix_rotation(disp, true);
lv_draw_ortho_take_sync(disp);             /* LVGL's buffer copies, made only where needed */
lv_draw_ortho_set_cores(2);                /* from LVGL's task: every other tile on core 1 */
```

The flush callback receives areas in the panel's orientation: with the panel's
own frame buffers there is nothing to copy, the last area of a frame shows it.
`examples/waveshare_7b` is a complete application for the Waveshare
ESP32-P4-WIFI6-Touch-LCD-7B; for another board, replace its `example_lcd.c`.

## API

| | |
|---|---|
| `lv_draw_ortho_init()` | create the draw unit, after `lv_init()` |
| `lv_draw_ortho_ppa_init()` / `_deinit()` | the PPA as turner and filler |
| `lv_draw_ortho_ppa_use_tiles(bool)` | big PSRAM pictures through the first tile, lent as two bands |
| `lv_draw_ortho_ppa_set_stage(bytes)` | the same through bands of their own, in internal RAM |
| `lv_draw_ortho_set_cores(n)` | paint every other tile on the second core |
| `lv_draw_ortho_take_sync(disp)` | take over LVGL's copies between DIRECT-mode frame buffers |
| `lv_draw_ortho_set_image_cache(bytes)` | PSRAM budget for pictures turned once and kept |
| `lv_draw_ortho_video_*` | a video window turned and shown by the PPA, outside LVGL |
| `lv_draw_ortho_get_stats()` | what was drawn and turned, and how |

Each function is described in its header.

## Tested

The repository's host tests draw every scene with LVGL's own renderer and with
lv_draw_ortho on a fake PPA, and compare the frame buffers pixel for pixel: 677
cases against LVGL 9.5.0 and 9.6.0, on one core and two, under ThreadSanitizer
and AddressSanitizer, with mutants that each remove one wait or guard and must
make a case fail. The figures above are from the boards.

## License

MIT. See `LICENSE`.

### Experimental resource lifecycle

On the experimental-resource-lifecycle branch only, an exclusive caller can
suspend between refreshes: check lv_draw_ortho_ppa_suspend(), then
lv_draw_ortho_suspend(). Do not render or call the draw-unit configuration APIs
until lv_draw_ortho_resume() succeeds. Reinitialize the PPA after resume, and
optionally enable the helper again. No video window or deferred sync hooks may
be active. A failed DMA drain is not permission to free anything. The unit,
job array and mutex remain allocated; the tile blocks and turned image cache
are released and helper tasks stopped. No new registry version is published.
