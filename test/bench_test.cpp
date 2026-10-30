// SPDX-License-Identifier: MIT
// Copyright (c) 2026 youkorr
// Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
//
// lvgl_bench (the shipped component) against a real LVGL on this machine:
// a whole-screen run must count frames and publish them, a quiet UI must
// publish nothing, and a frame must be timed from RENDER_START to the end
// of the flush, so a slow flush (a turn in it) shows in the figure.
#include "lvgl_bench.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>

namespace esphome {
static uint64_t now_us() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t) ts.tv_sec * 1000000u + ts.tv_nsec / 1000;
}
uint32_t millis() { return (uint32_t) (now_us() / 1000); }
uint32_t micros() { return (uint32_t) now_us(); }
}  // namespace esphome

using namespace esphome;
using namespace esphome::lvgl_bench;
static int g_failures, g_cases;
static void expect(bool ok, const char *what) {
  g_cases++;
  if (!ok) g_failures++;
  std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
}

static uint32_t g_flush_extra_us;
static void flush_cb(lv_display_t *d, const lv_area_t *, uint8_t *) {
  // Stands in for a turn done in the flush, as ESPHome's lvgl does.
  if (g_flush_extra_us) std::this_thread::sleep_for(std::chrono::microseconds(g_flush_extra_us));
  lv_display_flush_ready(d);
}
static uint32_t tick() { return millis(); }

static void spin(LvglBench &b, uint32_t ms) {
  const uint32_t end = millis() + ms;
  while ((int32_t) (millis() - end) < 0) {
    lv_timer_handler();
    b.loop();
  }
}

int main() {
  lv_init();
  lv_tick_set_cb(tick);
  const int w = 320, h = 480;
  static uint8_t buf[320 * 480 * 2];
  lv_display_t *d = lv_display_create(w, h);
  lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(d, buf, nullptr, sizeof(buf), LV_DISPLAY_RENDER_MODE_DIRECT);
  lv_display_set_flush_cb(d, flush_cb);
  lv_obj_t *l = lv_label_create(lv_screen_active());
  lv_label_set_text(l, "bench");
  lv_refr_now(d);

  sensor::Sensor ms, fps;
  LvglBench b;
  b.set_report_interval(200);
  b.set_ms_sensor(&ms);
  b.set_fps_sensor(&fps);
  b.setup();

  // A quiet UI: nothing redrawn, nothing published.
  spin(b, 450);
  expect(ms.count == 0 && fps.count == 0, "a quiet UI publishes nothing");

  // A whole-screen run through the shipped action.
  RunAction<> act;
  act.set_parent(&b);
  act.set_duration([]() -> uint32_t { return 300; });
  act.play();
  spin(b, 400);
  expect(ms.count == 1 && fps.count == 1, "a whole-screen run publishes once");
  expect(ms.state > 0 && fps.state > 0, "and with a real time and rate");
  const float fast = ms.state;
  std::printf("      %.2f ms a frame, at most %.0f fps\n", ms.state, fps.state);

  // The same with 5 ms added in the flush: the figure must carry it.
  g_flush_extra_us = 5000;
  act.play();
  spin(b, 400);
  std::printf("      with 5 ms in the flush: %.2f ms a frame\n", ms.state);
  expect(ms.count == 2 && ms.state >= fast + 4.5f, "the flush is inside the time");
  expect(fps.state > 0 && fps.state < 1000.0f / (fast + 4.5f) + 0.01f, "and the rate falls with it");

  std::printf("%d of %d cases passed\n", g_cases - g_failures, g_cases);
  return g_failures ? 1 : 0;
}
