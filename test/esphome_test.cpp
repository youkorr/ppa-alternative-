// SPDX-License-Identifier: MIT
// Copyright (c) 2026 youkorr
// Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
//
// The ESPHome component (components/lvgl_ortho/lvgl_ortho.cpp, the shipped
// file) run on a PC, end to end: an LVGL display built the way ESPHome's
// lvgl builds it -- the panel's size, a small partial buffer, its own flush
// -- is taken over by LvglOrtho::setup(), and a stand-in display::Display
// keeps every block draw_pixels_at() is given. What ends up on that glass
// must equal, pixel for pixel, the same UI rendered upright by LVGL and
// turned by LVGL's own lv_draw_sw_rotate -- first whole, then after a change
// that only needs a few rows, then with a camera frame shown in a canvas
// through show_video() and the (fake) PPA. A touch read through the wrapped
// input device must come back at the UI point drawn under it.
//
// One rotation per run: lv_draw_ortho_init() registers its draw unit once.
//   ./esphome_test 90     (ESPHome's degrees)

#include <lvgl.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "lvgl_ortho.h"

extern "C" {
#include "p4fake.h"
}

#if LVGL_VERSION_MAJOR > 9 || (LVGL_VERSION_MAJOR == 9 && LVGL_VERSION_MINOR >= 6)
#define turn_buffer lv_draw_rotate
#else
#define turn_buffer lv_draw_sw_rotate
#endif

namespace esphome {
static const auto T0 = std::chrono::steady_clock::now();
uint32_t millis() {
  return (uint32_t) std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - T0)
      .count();
}
uint32_t micros() {
  return (uint32_t) std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - T0)
      .count();
}
}  // namespace esphome

using esphome::display::Display;
using esphome::lvgl_ortho::LvglOrtho;

static int g_fail = 0, g_cases = 0;
static void expect(bool ok, const char *what) {
  g_cases++;
  if (!ok)
    g_fail++;
  std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
}

static lv_display_rotation_t lv_rot(int deg) {
  return deg == 90 ? LV_DISPLAY_ROTATION_270 : deg == 270 ? LV_DISPLAY_ROTATION_90
         : deg == 180 ? LV_DISPLAY_ROTATION_180 : LV_DISPLAY_ROTATION_0;
}

// A camera frame: every pixel different, so a misplaced or mis-turned one
// cannot go unnoticed.
static lv_draw_buf_t *make_frame(int w, int h, uint16_t seed) {
  lv_draw_buf_t *f = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
  for (int y = 0; y < h; y++) {
    uint16_t *row = (uint16_t *) (f->data + y * f->header.stride);
    for (int x = 0; x < w; x++)
      row[x] = (uint16_t) (seed ? seed : (x * 7 + y * 131 + 1));
  }
  return f;
}

struct Scene {
  lv_obj_t *box, *label, *small, *canvas;
};

// An ordinary LVGL page: a coloured ground, a label, a button with rounded
// corners and a border, a small square near one corner, and the camera's
// image widget. No lines: LVGL's own line renderer loses a pixel at a band
// cut (see --lvgl-quirk), which would be LVGL's difference, not ours.
static Scene build(lv_obj_t *scr, int uw, int uh, lv_draw_buf_t *camera) {
  Scene s;
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x203040), 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  s.box = lv_button_create(scr);
  lv_obj_set_pos(s.box, uw / 10, uh / 12);
  lv_obj_set_size(s.box, uw / 3, uh / 6);
  lv_obj_set_style_radius(s.box, 9, 0);
  lv_obj_set_style_border_width(s.box, 3, 0);
  lv_obj_set_style_border_color(s.box, lv_color_hex(0xffcc00), 0);
  s.label = lv_label_create(scr);
  lv_label_set_text(s.label, "Portall 12:34");
  lv_obj_set_pos(s.label, uw / 2, 6);
  s.small = lv_obj_create(scr);
  lv_obj_remove_style_all(s.small);
  lv_obj_set_size(s.small, 12, 12);
  lv_obj_set_pos(s.small, uw - 20, uh - 20);
  lv_obj_set_style_bg_color(s.small, lv_color_hex(0x00ff00), 0);
  lv_obj_set_style_bg_opa(s.small, LV_OPA_COVER, 0);
  s.canvas = lv_image_create(scr);
  lv_obj_set_pos(s.canvas, uw / 8, uh / 3);
  lv_obj_set_size(s.canvas, uw / 2, uh / 2);
  lv_image_set_src(s.canvas, camera);
  return s;
}

static void no_flush(lv_display_t *d, const lv_area_t *, uint8_t *) { lv_display_flush_ready(d); }

// What the board shows today, by LVGL alone: upright, then turned.
static std::vector<uint8_t> reference(int nw, int nh, int deg, lv_draw_buf_t *camera, void (*change)(Scene &)) {
  const bool across = deg == 90 || deg == 270;
  const int uw = across ? nh : nw, uh = across ? nw : nh;
  lv_display_t *up = lv_display_create(uw, uh);
  lv_display_set_color_format(up, LV_COLOR_FORMAT_RGB565);
  const uint32_t stride = uw * 2;
  std::vector<uint8_t> buf(size_t(stride) * uh + 128);
  uint8_t *b = (uint8_t *) (((uintptr_t) buf.data() + 127) & ~(uintptr_t) 127);
  lv_display_set_buffers_with_stride(up, b, nullptr, stride * uh, stride, LV_DISPLAY_RENDER_MODE_DIRECT);
  lv_display_set_flush_cb(up, no_flush);
  lv_display_set_default(up);
  Scene s = build(lv_display_get_screen_active(up), uw, uh, camera);
  if (change)
    change(s);
  lv_refr_now(up);
  std::vector<uint8_t> out(size_t(nw) * nh * 2);
  if (deg == 0)  // LVGL's rotate does nothing for no rotation
    std::memcpy(out.data(), b, out.size());
  else
    turn_buffer(b, out.data(), uw, uh, stride, nw * 2, lv_rot(deg), LV_COLOR_FORMAT_RGB565);
  lv_display_delete(up);
  return out;
}

static size_t diffs(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b) {
  size_t n = 0;
  for (size_t i = 0; i + 1 < a.size(); i += 2)
    n += a[i] != b[i] || a[i + 1] != b[i + 1];
  return n;
}

static void small_change(Scene &s) { lv_obj_set_style_bg_color(s.small, lv_color_hex(0xff00ff), 0); }

// The touchscreen of an ESPHome board: it reports the GLASS's coordinates.
static lv_point_t g_glass_point;
static void glass_read(lv_indev_t *, lv_indev_data_t *data) {
  data->point = g_glass_point;
  data->state = LV_INDEV_STATE_PRESSED;
}

int main(int argc, char **argv) {
  const int deg = argc > 1 ? std::atoi(argv[1]) : 90;
  const int nw = argc > 2 ? std::atoi(argv[2]) : 320, nh = argc > 3 ? std::atoi(argv[3]) : 480;
  std::printf("lvgl_ortho on a %dx%d panel at ESPHome rotation %d\n", nw, nh, deg);
  lv_init();
  const bool across = deg == 90 || deg == 270;
  const int uw = across ? nh : nw, uh = across ? nw : nh;
  // The camera frame is larger than its canvas, so show_video() must crop
  // it centred, as the image widget does.
  const int fw = uw / 2 + 30, fh = uh / 2 + 14;
  lv_draw_buf_t *frame = make_frame(fw, fh, 0);
  lv_draw_buf_t *blank = make_frame(fw, fh, 0x1234);

  std::vector<uint8_t> want = reference(nw, nh, deg, blank, nullptr);
  std::vector<uint8_t> want_changed = reference(nw, nh, deg, blank, small_change);
  std::vector<uint8_t> want_video = reference(nw, nh, deg, frame, small_change);

  // The board: ESPHome's lvgl has built a panel-sized display with a small
  // partial buffer and its own flush (which would turn); no rotation on it.
  lv_display_t *disp = lv_display_create(nw, nh);
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
  static uint8_t partial[64 * 1024];
  lv_display_set_buffers(disp, partial, nullptr, sizeof(partial), LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, no_flush);
  lv_display_set_default(disp);
  lv_indev_t *touch = lv_indev_create();
  lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(touch, glass_read);
  lv_indev_set_display(touch, disp);
  // The page is built at the panel's size, as ESPHome's lvgl builds it, and
  // laid out again when lvgl_ortho turns the display.
  Scene s = build(lv_display_get_screen_active(disp), uw, uh, blank);

  Display panel(nw, nh);
  LvglOrtho ortho;
  ortho.set_display(&panel);
  ortho.set_rotation(deg);
  ortho.setup();
  expect(!ortho.is_failed() && ortho.is_ready(), "setup() took the display over");
  expect(lv_display_get_horizontal_resolution(disp) == uw && lv_display_get_vertical_resolution(disp) == uh,
         "LVGL now sees the UI's size");
  lv_obj_set_pos(s.box, uw / 10, uh / 12);  // what a re-laid-out page would hold
  lv_refr_now(disp);
  char what[200];
  size_t d = diffs(panel.glass, want);
  std::snprintf(what, sizeof what, "the glass equals LVGL's upright UI turned (%zu pixels differ, %d draws)", d,
                panel.calls);
  expect(d == 0 && panel.bad == 0, what);

  // A change in one small square: only a few rows should go out.
  const int calls = panel.calls;
  const uint64_t pixels = panel.pixels;
  small_change(s);
  lv_refr_now(disp);
  d = diffs(panel.glass, want_changed);
  std::snprintf(what, sizeof what, "a small change: glass right (%zu differ), %llu pixels sent of %d", d,
                (unsigned long long) (panel.pixels - pixels), nw * nh);
  expect(d == 0 && panel.calls > calls && panel.pixels - pixels < uint64_t(nw) * nh / 4, what);

  // The camera, through the PPA, in the canvas's place. LVGL holds the blank
  // frame as the image's source and must never draw it over the video.
  const int srm = g_p4.srm_count;
  bool shown = ortho.show_video(s.canvas, frame->data, fw, fh);
  lv_obj_invalidate(lv_display_get_screen_active(disp));  // and LVGL redraws everything around it
  lv_refr_now(disp);
  d = diffs(panel.glass, want_video);
  std::snprintf(what, sizeof what, "a camera frame through show_video(): glass right (%zu differ), %d PPA pass",
                d, g_p4.srm_count - srm);
  expect(shown && d == 0 && g_p4.srm_count - srm == 1, what);

  // A finger: the glass point under a UI point must come back as that point.
  const lv_point_t ui = {uw - 14, uh - 14};  // the small square's middle
  lv_area_t a = {ui.x, ui.y, ui.x, ui.y};
  lv_area_t n;
  lv_draw_ortho_native_area(disp, &a, &n);
  g_glass_point = {n.x1, n.y1};
  lv_indev_data_t data;
  std::memset(&data, 0, sizeof data);
  lv_indev_get_read_cb(touch)(touch, &data);
  std::snprintf(what, sizeof what, "touch at glass %d,%d reads UI %d,%d (want %d,%d)", (int) n.x1, (int) n.y1,
                (int) data.point.x, (int) data.point.y, (int) ui.x, (int) ui.y);
  expect(data.point.x == ui.x && data.point.y == ui.y, what);

  std::printf("%d of %d cases passed\n", g_cases - g_fail, g_cases);
  return g_fail ? 1 : 0;
}
