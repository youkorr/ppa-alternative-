# ppa-alternative — `lv_draw_ortho`

**Draw a turned LVGL display in the panel's own orientation, instead of
drawing it upright and turning every frame.**

A panel used the other way round from how it was built — an ESP32-P4 Tab5 or
Guition in landscape, any portrait MIPI or RGB panel on its side — costs LVGL
two passes over every changed pixel today: it renders upright into a buffer,
then the flush callback turns that buffer (in software, or with a 2D
accelerator such as the ESP32-P4's PPA) into the frame buffer. The second pass
is a transpose, the worst access pattern a memory bus can be given, and for
full-screen motion it is what sets the frame rate. On a Tab5 a full-screen
picture through LVGL at 270° measured about **8 frames a second, with or
without the PPA**.

LVGL 9 already has the right idea: `lv_display_set_matrix_rotation()` hands
each draw task a matrix and lets the draw unit render through it. But only the
vector GPU units read that matrix; with the software renderer it draws
nothing. **`lv_draw_ortho` is a draw unit that reads it**, for the four
quarter turns, on any CPU:

| what is drawn | how |
|---|---|
| a solid fill (any radius) | drawn as the native rectangle it becomes — no turn at all |
| an opaque image, unscaled | copied **once**, turned on the way, in cache-sized blocks |
| an image already stored native (`LV_DRAW_ORTHO_IMAGE_FLAG_NATIVE`) | copied row by row — **no turn**, what a video or camera source can provide |
| everything else (text, borders, shadows, arcs, lines, masks, layers, blur…) | LVGL's own software renderer draws it into a small upright tile, which is put back turned |

The last line is why the result is **pixel-identical to LVGL**: anything that
is not a fast path is drawn by LVGL's renderer itself, so there is nothing to
re-implement and nothing to drift.

## Status

**Verified on a PC, not yet on a board.**

- `test/` builds LVGL **9.5.0** and renders the same UI two ways: upright and
  then turned with `lv_draw_sw_rotate` (what a flush callback does today), and
  straight to native with `lv_draw_ortho`. **54 of 54 cases are byte-identical**:
  90, 180 and 270°, RGB565 and XRGB8888, two panel sizes (one odd), direct
  and partial render modes, fast paths on and off, plus pre-turned pictures.
  The scene has gradients both ways, a shadow, borders, rounded and circular
  buttons, text in two fonts, an arc, a line, opaque and alpha images, an image
  hanging off the edge, a slider and a translucent layer.
- The test catches a broken unit: three deliberately broken copies (a
  half-pixel error, swapped axes, an unturned fill) each fail it.
- Clean under AddressSanitizer and UBSan.
- One full-screen 720×1280 picture at 270°, on the build machine (an x86
  server, **not an ESP32-P4**; only the ratios mean anything):

  | | time |
  |---|---|
  | LVGL today: draw upright, turn in flush | 1.22 ms |
  | `lv_draw_ortho`, turned in one pass | 0.56 ms (2.2× faster) |
  | `lv_draw_ortho`, picture already native | 0.17 ms (7× faster) |

  The lesson in those numbers is that **the turn itself is the cost**. One pass
  instead of two halves it; not turning at all removes it. A video decoder or a
  camera pipeline that writes its frames in the panel's orientation gets the
  third line.

What is **not** done yet:

- Running on an ESP32-P4, and a number from one.
- A copy callback (`lv_draw_ortho_set_copy_cb`) backed by the P4's 2D-DMA for
  the straight copies, so the CPU is free during them.
- ESPHome glue: its `lvgl:` component turns in its flush; with this unit the
  display is created with `lv_display_set_rotation()` +
  `lv_display_set_matrix_rotation(true)` and the flush copies straight.
- Objects with a free transform angle (`transform_rotation: 300`) under a
  turned display: the matrix is then not a quarter turn, and this unit counts
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
`LV_DRAW_TRANSFORM_USE_MATRIX 1`.

**Partial render mode** (a small buffer, which is what ESPHome uses) needs
`patches/lvgl-9.5.0-matrix-rotation-partial.patch` on LVGL: LVGL 9.5 allows
matrix rotation only in direct and full modes. The patch is three small hunks:
allow the mode, size the partial buffer for the turned area, and flush the
turned area. With it, the flush callback receives the native area and copies
it as it is. It is meant to go upstream with this unit.

A pre-turned picture: keep `header.w` × `header.h` as the size the UI sees,
store the rows as they will be on the glass, give the native row length in
`header.stride`, and set `LV_DRAW_ORTHO_IMAGE_FLAG_NATIVE` in `header.flags`.

`lv_draw_ortho_get_stats()` says which path each frame took.

## Running the tests

```
git clone --depth 1 -b v9.5.0 https://github.com/lvgl/lvgl /tmp/lvgl
cd test
make LVGL=/tmp/lvgl run      # 54 cases
make LVGL=/tmp/lvgl bench    # timings
```

The LVGL tree is copied and patched inside `test/build`, never modified.

## Going upstream

The shape is chosen for LVGL: one draw unit, no platform code, LVGL's own
`lv_matrix_t` and software renderer, and a test that compares against LVGL's
own output. The partial-mode patch is a separate, small change to `lv_refr.c`
and `lv_display.c`. Nothing is specific to Espressif — any MCU driving a panel
on its side has the same problem.

## Licence

MIT, like LVGL.
