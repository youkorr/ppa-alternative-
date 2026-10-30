# ppa-alternative — `lv_draw_ortho`

**Draw a turned LVGL display in the panel's own orientation, instead of
drawing it upright and turning every frame.**

A panel used the other way round from how it was built — an ESP32-P4 Tab5 or
Guition in landscape, any portrait MIPI or RGB panel on its side — costs LVGL
two passes over every changed pixel today: it renders upright into a buffer,
then the flush callback turns that buffer (in software, or with a 2D
accelerator such as the ESP32-P4's PPA) into the frame buffer. The second pass
is a transpose, the worst access pattern a memory bus can be given, and for
full-screen motion it is what sets the frame rate. On a Tab5, a full-screen
picture through LVGL at 270° measured about **8 frames a second, with or
without the PPA**.

LVGL 9 already has the right idea: `lv_display_set_matrix_rotation()` hands
each draw task a matrix and lets the draw unit render through it. But only the
vector GPU units read that matrix. In **9.5 and 9.6** alike, the software
renderer and the ESP32-P4 PPA unit ignore it, so on those the option draws
nothing. **`lv_draw_ortho` is a draw unit that reads it**, for the four quarter
turns, on any CPU:

| what is drawn | how |
|---|---|
| an opaque image, unscaled | copied **once**, turned on the way, in cache-sized blocks |
| an image already stored native (`LV_DRAW_ORTHO_IMAGE_FLAG_NATIVE`) | copied row by row, **no turn** — what a video or camera source can provide |
| everything else (fills, text, borders, shadows, arcs, lines, masks, layers…) | a run of tasks is drawn by LVGL's own software renderer into one upright tile, which is turned **once** into the frame buffer |

Three things make the general path cheaper than LVGL's own way, not just equal
to it:
- **Runs, not tasks.** All the tasks of a screen go through the same tiles, so
  each pixel is turned once, however many widgets are stacked on it.
- **No read-back under an opaque fill.** A tile whose first task paints it
  entirely (a screen background, a card) is never read from the frame buffer.
- **Holes.** A border never touches the middle of its rectangle, and a shadow
  under an opaque background is painted over by it, so those pixels are not
  turned at all.

Anything that is not a fast path is drawn by LVGL's renderer itself, so there
is nothing to re-implement and nothing to drift: the result is LVGL's, pixel
for pixel.

## Status

**Verified on a PC against LVGL 9.5.0 and 9.6.0, not yet on a board.**

- `test/` builds LVGL and renders the same UI two ways: upright and then
  turned (what a flush callback does today), and straight to native with
  `lv_draw_ortho`. **70 of 70 cases match on both 9.5.0 and 9.6.0**: 90, 180
  and 270°, RGB565 and XRGB8888, two panel sizes (one odd), direct and partial
  render modes, fast paths on and off, batched and task by task, plus
  pre-turned pictures. The scene has gradients both ways, a shadow, borders,
  rounded and circular buttons, text in two fonts, an arc, a polyline, opaque
  and alpha images, an image hanging off the edge, a slider and a translucent
  group.
- **Exactly one pixel, in three cases, is excused, and only because LVGL draws
  it the same way.** LVGL's own line renderer is not independent of where an
  area is cut: `make bench` shows nothing, but `build/ortho_test --lvgl-quirk`
  shows LVGL **alone** losing an anti-aliased edge pixel of a polyline when a
  partial band ends on the joint (`ce79` drawn whole, `f7be` drawn in bands of
  51 rows). Tiles cut too, so they can meet the same thing. The test excuses a
  differing pixel **only** when LVGL itself, made to cut its bands on that
  pixel's row, draws exactly what `lv_draw_ortho` drew. Anything else fails.
  This is worth reporting to LVGL on its own.
- The test catches a broken unit: six deliberately broken copies (a half-pixel
  error, an unturned fill, never reading back, a border hole that swallows the
  border, a batch drawn in reverse, no margin for glyphs) each fail it.
- Clean under AddressSanitizer and UBSan.
- Timings, LVGL 9.6.0, 720×1280 at 270°, RGB565, on the build machine (an x86
  server, **not an ESP32-P4**; only the ratios mean anything):

  | | LVGL today (upright + turn in flush) | `lv_draw_ortho` |
  |---|---|---|
  | one full-screen picture | 1.23 ms | **0.55 ms** turned in one pass (2.2×) |
  | the same, picture already native | 1.23 ms | **0.17 ms** (7×) |
  | the whole test UI redrawn | 1.71 ms | **1.45 ms** (1.2×) |

  LVGL 9.6's rewritten software renderer made blending faster but not the
  turn: the full-screen figure is the same as on 9.5.

  For a picture, the lesson is that **the turn itself is the cost**. One pass
  instead of two halves it, and not turning at all removes it. A video decoder
  or camera pipeline that writes its frames in the panel's orientation gets the
  last line.

What is **not** done yet:

- Running on an ESP32-P4, and a number from one.
- A copy callback (`lv_draw_ortho_set_copy_cb`) backed by the P4's 2D-DMA for
  the straight copies, so the CPU is free during them.
- ESPHome glue: its `lvgl:` component turns in its flush. With this unit the
  display is created with `lv_display_set_rotation()` and
  `lv_display_set_matrix_rotation(true)`, and the flush copies straight.
- Objects with a free transform angle (`transform_rotation: 300`) under a
  turned display: the matrix is then not a quarter turn, so this unit counts
  the task in `tasks_refused` rather than drawing it wrongly.

## Use

```c
#include "lv_draw_ortho.h"

lv_init();
lv_draw_ortho_init();                       /* after lv_init() */

lv_display_t * d = lv_display_create(720, 1280);   /* the PANEL's size */
lv_display_set_buffers(d, buf, NULL, size, LV_DISPLAY_RENDER_MODE_DIRECT);
lv_display_set_rotation(d, LV_DISPLAY_ROTATION_270);
lv_display_set_matrix_rotation(d, true);
/* flush_cb: the buffer is already native -- copy it, never turn it. */
```

`lv_conf.h` needs `LV_USE_FLOAT 1`, `LV_USE_MATRIX 1` and
`LV_DRAW_TRANSFORM_USE_MATRIX 1`. The private headers are reached through
`lvgl_private.h`, which both 9.5 and 9.6 provide.

`LV_DRAW_ORTHO_TILE_BYTES` (128 KiB by default) is the one setting that
matters. Every tile costs each task touching it a call into the software
renderer, and a label lays its text out again, so a tile that is too small is
slower than LVGL's own way. With the test UI: 32 KiB 2.6 ms, 64 KiB 1.8 ms,
128 KiB 1.45 ms, 256 KiB 1.3 ms, against 1.7 ms for LVGL today.

**Partial render mode** (a small buffer, which is what ESPHome uses) needs the
patch for your LVGL in `patches/`: neither 9.5 nor 9.6 allows matrix rotation
outside direct and full modes. The patch is three small hunks: allow the mode,
size the partial buffer for the turned area, and flush the turned area. With
it, the flush callback receives the native area and copies it as it is. It is
meant to go upstream with this unit.

A pre-turned picture: keep `header.w` × `header.h` as the size the UI sees,
store the rows as they will be on the glass, give the native row length in
`header.stride`, and set `LV_DRAW_ORTHO_IMAGE_FLAG_NATIVE` in `header.flags`.

`lv_draw_ortho_get_stats()` says which path each frame took.

## Running the tests

```
git clone --depth 1 -b v9.6.0 https://github.com/lvgl/lvgl /tmp/lvgl
cd test
make LVGL=/tmp/lvgl run            # 70 cases
make LVGL=/tmp/lvgl bench          # timings
build/ortho_test --lvgl-quirk      # LVGL alone, drawn whole and in bands
```

The LVGL tree is copied and the patch for its exact version applied inside
`test/build`, so the tree given is never modified. 9.5.0 works the same way.

## Going upstream

The shape is chosen for LVGL: one draw unit, no platform code, LVGL's own
`lv_matrix_t` and software renderer, and a test that compares against LVGL's
own output. Nothing is specific to Espressif: any MCU driving a panel on its
side has the same problem. Three separate things to propose:

1. the draw unit, as the software renderer's answer to matrix rotation;
2. the partial-mode patch to `lv_refr.c` and `lv_display.c`;
3. the line renderer losing a pixel at a band cut, with `--lvgl-quirk` as
   the reproduction.

## Licence

MIT, like LVGL.
