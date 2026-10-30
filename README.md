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

**Verified on a PC against LVGL 9.5.0, 9.6.0 and `master` (10.0.0-dev),
not yet on a board.**

- `test/` builds LVGL and renders the same UI two ways: upright and then
  turned (what a flush callback does today), and straight to native with
  `lv_draw_ortho`. **136 of 136 cases match on 9.5.0, 9.6.0 and `master`**: 90, 180
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

### The ESP32-P4 camera (esp-video)

`lvgl_camera_display` in `youkorr/esphome_esp-video` shows the camera without
copying it: one `lv_draw_buf_t` pointed at the camera's DMA buffer, given to a
canvas with `lv_canvas_set_draw_buf()`, its `.data` changed for every frame.
That is exactly the shape this unit's fast path takes, an opaque unscaled
RGB565 image, so **a camera frame goes from the DMA buffer to the frame buffer
in one pass**, where today LVGL copies it into its own buffer and then turns
that in the flush. `test/` does what that component does, frame after frame,
and checks every frame against LVGL: 12 cases, at 90, 180 and 270°, frames
smaller and bigger than the screen, upright and pre-turned.

Timed on the households' own boards' geometry (the build machine again, not a
P4):

| board (esp-video `yaml/`) | camera | LVGL today | turned in one pass | frame pre-turned |
|---|---|---|---|---|
| Guition 10", 800×1280 at 90° (`tab10`) | 1288×728 | 1.35 ms | 0.57 ms (2.4×) | 0.19 ms (7.1×) |
| Tab5 in landscape, 720×1280 at 270° | 1280×720 | 1.42 ms | 0.57 ms (2.5×) | 0.19 ms (7.5×) |
| Waveshare 7", 1024×600 at 180° (`lvgl-kawaii`) | 1280×960 | 0.31 ms | 0.34 ms (0.9×) | 0.11 ms (2.8×) |

A half turn is a copy read backwards, which is cheap for LVGL too, so at 180°
the gain needs the frames pre-turned. That one is nearly free: a 180° native
frame is the frame mirrored both ways, which the camera's own PPA pass does
with `mirror_x: true` and `mirror_y: true` (not the sensor's own flip, which
pixellates the OV02C10's Bayer pattern, as `ov02c10_custom_formats.h` notes).
At 90 and 270°, the camera's `rotation:` makes the same PPA pass turn the
frame. Either way the turn moves out of LVGL's flush and into the camera's one
hardware pass, and the CPU only copies rows.

On a Tab5 today, LVGL takes 50-73 ms per camera frame (10-15 fps). How much of
that this removes is what a board will say: one memory pass instead of two or
three is the mechanism, not a measured frame rate.

What is **not** done yet:

- Running on an ESP32-P4, and a number from one.
- A copy callback (`lv_draw_ortho_set_copy_cb`) backed by the P4's 2D-DMA for
  the straight copies, so the CPU is free during them.
- ESPHome glue: its `lvgl:` component turns in its flush. With this unit the
  display is created with `lv_display_set_rotation()` and
  `lv_display_set_matrix_rotation(true)`, and the flush copies straight.
- In `lvgl_camera_display`, marking a camera frame
  `LV_DRAW_ORTHO_IMAGE_FLAG_NATIVE` (with the native stride) when the camera
  turned it. It must only ever be set with this unit running: LVGL alone
  would read the rows the wrong way.
- Objects with a free transform angle (`transform_rotation: 300`) under a
  turned display: the matrix is then not a quarter turn, so this unit counts
  the task in `tasks_refused` rather than drawing it wrongly.

## Video at the sensor's frame rate

**How Espressif and M5Stack do it**, read in their sources rather than
assumed:

- **Espressif** (`examples/peripherals/camera/mipi_isp_dsi` in ESP-IDF): the
  camera controller's DMA is pointed **at the DSI panel's frame buffer**
  (`example_dsi_resource_alloc(..., &frame_buffer, ...)`, then
  `new_trans.buffer = frame_buffer`). No copy, no CPU, no LVGL, no turn: the
  sensor's frames land on the glass at the sensor's rate.
- **M5Stack** (Tab5 user demo, `hal_camera.cpp`): one PPA pass (a mirror)
  from the camera's buffer into an LVGL canvas; LVGL renders in 50-row bands
  in **internal** RAM (`buff_spiram = false`), and `esp_lvgl_port` turns each
  band into the frame buffer with the PPA.

The best of both is what `components/lvgl_ortho/lv_draw_ortho_video.c` does:

```
camera DMA -> camera buffer --(ONE PPA pass: turn, mirror, scale)--> frame buffer window
LVGL      -> lv_draw_ortho: the rest of the screen, straight into the same frame buffer, never the window
```

Espressif's zero copy, with a UI around it and any panel orientation. LVGL
never handles a video pixel; the CPU handles none at all. `lv_draw_ortho_set_window()`
is the half that makes it safe: every write this unit makes into the frame
buffer -- tiles, fills, images -- is cut around the window.

```c
lv_draw_ortho_video_t v;
lv_draw_ortho_video_init(&v, disp);              /* a PPA SRM client */
lv_draw_ortho_video_place(&v, &obj_coords);      /* where, in UI coordinates */
/* from LVGL's own thread, for every camera frame: */
lv_draw_ortho_video_show(&v, cam_buf, 1280, 720, LV_COLOR_FORMAT_RGB565, true, false);
```

**The household's three camera boards**, read in esp-video's `yaml/` and in
the sensor drivers it builds (`components/esp_cam_sensor/sensor/*`). A
sensor's MIPI lanes and frame rate belong to the format, not to anything
downstream, so the rate in this table is the ceiling the display path has to
keep up with:

| board (YAML) | panel | ESPHome `rotation:` | = LVGL | sensor | format in use | lanes | sensor rate | shown |
|---|---|---|---|---|---|---|---|---|
| M5Stack Tab5 (`habbit (1).yaml`) | 720×1280 | 270° | `LV_DISPLAY_ROTATION_90` | SC2356 (SC202CS silicon) | RAW8 1280×720 | 1 | 30 fps | the whole frame, the whole screen |
| Guition 10" (`tab10 .yaml`) | 800×1280 | 90° | `LV_DISPLAY_ROTATION_270` | OV02C10 | RAW10 1288×728 | 1 | 30 fps | its centre 1280×720 at 1:1, under 80 px of UI |
| Waveshare 7" (`lvgl-kawaii.yaml`) | 1024×600 | 180° | `LV_DISPLAY_ROTATION_180` | OV5647 | RAW10 1280×960 binned | 2 | 45 fps (the YAML asks 30) | its centre 1024×600 at 1:1, the whole screen |

**ESPHome and LVGL count their quarter turns opposite ways**, and that is not
a detail: ESPHome's `rotation: 90` turns the picture clockwise (its own
software loop sends the top-left pixel to the top-right), LVGL's
`LV_DISPLAY_ROTATION_90` counter-clockwise (`lv_draw_rotate` sends it to the
bottom-left). The PPA's angles are counter-clockwise on the screen, as its
documentation says, so its `ANGLE_90` is LVGL's 90 and ESPHome's 270.

All three are cases in `test/`, at those real sizes: the screen with the
camera written by the (fake) PPA is the screen LVGL draws with that crop of
the frame as an ordinary image. `lv_draw_ortho_video_set_crop()` is what the
Guition and the Waveshare need: the PPA reads only the block shown, in the
same pass, so a 1288×728 frame on a 1280×800 screen stays sharp instead of
being scaled by 0.99.

**The cache, which is where this goes wrong if it does.** LVGL writes right up
to the window's edge, so a cache line can hold LVGL pixels and stale video
pixels at once; written back after the PPA has run, it puts the stale video
back. So before every PPA pass the window's rows are written back and
invalidated together (`C2M | INVALIDATE`, on whole L2 lines), and the PPA
writes into lines nobody holds. That only works if LVGL is not rendering at
the same moment, which is why `show()` belongs on LVGL's own thread.

**Verified on a PC**, with `test/p4stub/` standing in for ESP-IDF's PPA and
cache: the declarations are copied from v5.5.5 field for field, and the fake
PPA turns the way the household's boards do. That is itself a test: the
rotation loops of `youkorr/lvgl_9.5` (`lvgl_esphome.cpp`), which show a correct
UI on the Tab5 through the PPA path beside them, are copied, and the fake PPA
at the angle that fork gives the PPA must produce exactly what those loops
produce, at ESPHome's 90, 180 and 270 -- and LVGL's own `lv_draw_rotate` at the
opposite angle must too. An earlier version of this file had the quarter
turns of the PPA the wrong way round, reasoned from `esp_lvgl_port`; on a
board the camera would have come out upside down on the Tab5 and the Guition.
That check fails it. 12 cases: the screen with the camera
written by the PPA is the screen LVGL draws with that frame as an ordinary
image, at 0, 90, 180 and 270°, mirrored and not; a full LVGL redraw afterwards
leaves it alone; one PPA pass; the cache synced on whole lines before it.
Broken on purpose, each of a wrong angle, a missing cache sync and an unswapped
mirror axis fails it.

**Not known until a board says**:

- Whether the PPA mirrors before or after it turns. The code assumes after
  (the order of its name, scale-rotate-mirror) and swaps the mirror's axis at
  90° and 270°. A mirrored camera flipped the wrong way on a turned panel
  means this is the line.
- How fast the PPA writes into a PSRAM frame buffer the DSI is reading at the
  same time; the Waveshare 7B measured 14.9 ms for a whole 1024x600 turned.
- Tearing: with one frame buffer, the scan-out can catch a frame half written,
  as in Espressif's example.
- Scaling: frames the size of the window are exact; the PPA rounds a scaled
  block down, which can leave a 1-pixel edge (lvgl/lvgl#10265 patches that
  case for its own images).

## On an ESPHome board

`components/lvgl_ortho` is an ESPHome external component. Two lines change in
a board's YAML: `rotation:` leaves the `lvgl:` block and goes on `lvgl_ortho:`,
with the same number.

```yaml
external_components:
  - source: github://youkorr/ppa-alternative-@main
    components: [lvgl_ortho]

lvgl:
  # no rotation: here any more
  buffer_size: 10%        # LVGL's own buffer is replaced; keep it small

lvgl_ortho:
  id: ortho
  rotation: 270           # what lvgl: had (ESPHome's degrees, clockwise)
```

What it does once ESPHome's lvgl has built the display:

- LVGL renders into **one panel-sized buffer in the panel's orientation**
  (`LV_DISPLAY_RENDER_MODE_DIRECT`, PSRAM), through `lv_draw_ortho`, so
  nothing is turned after it is drawn;
- the flush copies the rows that changed to the display as they are (a
  narrow area is packed first, so a clock along one edge is not a whole
  panel of copying);
- touches are turned back to the UI's coordinates (`lv_display_rotate_point`),
  since LVGL 9 does not turn an input device's points itself;
- the three LVGL defines it needs are written into ESPHome's `lv_conf.h` --
  after the lvgl component's own code, because youkorr/lvgl_9.5 sets
  `LV_USE_MATRIX` to 0 when no Lottie or SVG is used.

**The camera.** `show_video(obj, frame, w, h)` turns a frame with one PPA pass
straight into a window of that buffer, where LVGL does not draw, and puts it
on the glass. youkorr/esphome_esp-video's `lvgl_camera_display` does it with
one line, `ortho_id: ortho`. `example/guition-lvgl-ortho.yaml` is a whole
board, validated with `esphome config` on ESPHome 2026.10.0.

**Tested on a PC, end to end.** `test/esphome_test.cpp` runs the shipped
`lvgl_ortho.cpp` on an LVGL display built the way ESPHome's lvgl builds it,
with a stand-in display that keeps every `draw_pixels_at()`: the glass equals
LVGL's upright UI turned, pixel for pixel, at 0, 90, 180 and 270, at 320x480
and at the Tab5's and the Guition's real sizes -- whole, after a small change
(484 pixels sent, not a panel), and with a camera frame through the fake PPA.
A touch on the glass reads back as the UI point drawn there.
`test/esphome_mutants.py` breaks the component five ways (touch not turned,
flush area not turned, narrow area packed from the wrong column, ESPHome's 90
taken as LVGL's 90, frame not centred) and the test catches every one.

**Not compiled by ESP-IDF and not flashed.** What a board will say: the
`lvgl_ortho` lines in its log, and the frame rate. Unknown until then: PSRAM
bandwidth with the scanout, and tearing (the panel's frame buffer is still
ESPHome's, copied into by DMA).

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
`lvgl_private.h`, which 9.5, 9.6 and `master` all provide. On `master` the
software renderer draws a descriptor onto a layer within a clip, which is used
directly; on 9.x a copy of the task is pointed at the tile.

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
make LVGL=/tmp/lvgl run            # 136 cases, and the ESPHome component at four angles
make LVGL=/tmp/lvgl bench          # timings
build/ortho_test --lvgl-quirk      # LVGL alone, drawn whole and in bands
```

The LVGL tree is copied and the patch for its exact version applied inside
`test/build`, so the tree given is never modified. 9.5.0 works the same way.

## Going upstream

The shape is chosen for LVGL: one draw unit, no platform code, LVGL's own
`lv_matrix_t` and software renderer, and a test that compares against LVGL's
own output. Nothing is specific to Espressif: any MCU driving a panel on its
side has the same problem. A pull request to LVGL lands on `master`, which is
why the unit and the tests build there too.

**To LVGL**, three separate things:

1. the draw unit, as the software renderer's answer to matrix rotation;
2. the partial-mode patch to `lv_refr.c` and `lv_display.c`
   (`patches/lvgl-10.0.0-*.patch` for `master`);
3. the line renderer losing a pixel at a band cut, as a bug report, with
   `--lvgl-quirk` as the reproduction (present on 9.5, 9.6 and `master`).

**Beside lvgl/lvgl#10265** (the PPA unit's scaled and rotated images): it is
complementary rather than competing. That PR turns *an image*; this unit
stops turning *the screen*. Two things connect them:

- The PPA unit's `ppa_evaluate` does not look at the task's matrix, so with
  matrix rotation on and no `lv_draw_ortho` it would draw images unturned.
  With `lv_draw_ortho` the PPA unit gives those tasks up whatever the order
  the units are asked in, because this unit claims them at preference 0. One
  line in `ppa_evaluate` (refuse a task whose matrix is not the identity)
  makes the PPA unit correct on its own.
- On the ESP32-P4 the PPA is the natural engine for this unit's two copies:
  a turned copy is an SRM rotate, and a straight one an SRM copy, and the
  block geometry is what `lv_draw_ppa_rot_calc_block()` in #10265 computes.
  That is a back end for `lv_draw_ortho_set_copy_cb()` and a turned-copy
  callback still to add, and it needs a board to be measured on.

**To ESPHome**: ESPHome builds LVGL 9.5.0, so the unit can live in an
external component first, where it can be flashed and measured, and become
a `lvgl:` option once LVGL carries it. esphome/esphome#16944
(`esp_video_camera`, the camera as a Home Assistant entity) does not touch
the display; showing that camera on an LVGL screen is `lvgl_camera_display`
in esp-video, which is the case measured above.

## Licence

MIT, like LVGL.
