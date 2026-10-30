// SPDX-License-Identifier: MIT
// Copyright (c) 2026 youkorr
// Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
#pragma once

// How long LVGL takes to redraw the screen, timed on the board from
// LV_EVENT_RENDER_START to LV_EVENT_REFR_READY: the drawing, any turn and the
// flush to the panel, whichever way the display is set up. The same lines in
// a firmware on ESPHome's own lvgl and in one on lvgl_ortho compare the two.

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"

#include <lvgl.h>

namespace esphome {
namespace lvgl_bench {

class LvglBench : public Component {
 public:
  void set_report_interval(uint32_t ms) { this->interval_ms_ = ms; }
  void set_ms_sensor(sensor::Sensor *s) { this->ms_sensor_ = s; }
  void set_fps_sensor(sensor::Sensor *s) { this->fps_sensor_ = s; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  // After lvgl, which builds the display in its own setup().
  float get_setup_priority() const override { return setup_priority::LATE; }

  /// Redraw the whole active screen on every refresh for `ms`, then report.
  void run(uint32_t ms);

 protected:
  static void on_event_(lv_event_t *e);
  bool attach_();
  void report_(bool stress);

  uint32_t interval_ms_{10000};
  sensor::Sensor *ms_sensor_{nullptr};
  sensor::Sensor *fps_sensor_{nullptr};
  lv_display_t *disp_{nullptr};

  // Written from LVGL's events, which run on the loop's own thread.
  uint32_t started_us_{0};
  bool in_frame_{false};
  uint32_t frames_{0};
  uint64_t total_us_{0};
  uint32_t worst_us_{0};

  uint32_t window_start_{0};
  uint32_t stress_until_{0};
  bool stressing_{false};
};

template<typename... Ts> class RunAction : public Action<Ts...>, public Parented<LvglBench> {
 public:
  TEMPLATABLE_VALUE(uint32_t, duration)
  void play(const Ts &...x) override { this->parent_->run(this->duration_.value(x...)); }
};

}  // namespace lvgl_bench
}  // namespace esphome
