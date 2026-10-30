// SPDX-License-Identifier: MIT
// Copyright (c) 2026 youkorr
// Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
#include "lvgl_bench.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace lvgl_bench {

static const char *const TAG = "lvgl_bench";

bool LvglBench::attach_() {
  if (this->disp_ != nullptr)
    return true;
  lv_display_t *disp = lv_display_get_default();
  if (disp == nullptr)
    return false;
  this->disp_ = disp;
  // RENDER_START is sent only when something is redrawn; REFR_READY after
  // the flush has finished (lv_refr.c, LVGL 9.5), so a turn done in the
  // flush is inside the time.
  lv_display_add_event_cb(disp, on_event_, LV_EVENT_RENDER_START, this);
  lv_display_add_event_cb(disp, on_event_, LV_EVENT_REFR_READY, this);
  return true;
}

void LvglBench::setup() {
  this->window_start_ = millis();
  if (!this->attach_()) {
    ESP_LOGW(TAG, "No LVGL display yet; looking again from the loop");
  }
}

void LvglBench::on_event_(lv_event_t *e) {
  auto *self = static_cast<LvglBench *>(lv_event_get_user_data(e));
  const uint32_t now = micros();
  if (lv_event_get_code(e) == LV_EVENT_RENDER_START) {
    self->started_us_ = now;
    self->in_frame_ = true;
    return;
  }
  if (!self->in_frame_)
    return;  // a refresh with nothing redrawn
  self->in_frame_ = false;
  const uint32_t dt = now - self->started_us_;
  self->frames_++;
  self->total_us_ += dt;
  if (dt > self->worst_us_)
    self->worst_us_ = dt;
}

void LvglBench::run(uint32_t ms) {
  if (!this->attach_()) {
    ESP_LOGW(TAG, "No LVGL display to measure");
    return;
  }
  // Start a clean window so the result is the stress alone.
  this->frames_ = 0;
  this->total_us_ = 0;
  this->worst_us_ = 0;
  this->window_start_ = millis();
  this->stress_until_ = this->window_start_ + ms;
  this->stressing_ = true;
  ESP_LOGI(TAG, "Redrawing the whole screen on every refresh for %u ms", (unsigned) ms);
}

void LvglBench::loop() {
  if (!this->attach_())
    return;
  const uint32_t now = millis();
  if (this->stressing_) {
    if ((int32_t) (now - this->stress_until_) >= 0) {
      this->stressing_ = false;
      this->report_(true);
      return;
    }
    // The whole active screen, every turn of the loop: LVGL's refresh timer
    // then redraws all of it as often as it can.
    lv_obj_invalidate(lv_screen_active());
    return;
  }
  if (now - this->window_start_ >= this->interval_ms_)
    this->report_(false);
}

void LvglBench::report_(bool stress) {
  const uint32_t now = millis();
  const float secs = (now - this->window_start_) / 1000.0f;
  (void) secs;  // only in the log lines, which a low log level compiles out
  if (this->frames_ > 0) {
    const float ms = this->total_us_ / 1000.0f / this->frames_;
    const float max_fps = ms > 0 ? 1000.0f / ms : 0;
    ESP_LOGI(TAG, "%s: %u frames in %.1f s (%.1f/s), %.1f ms each, worst %.1f ms -> at most %.1f fps",
             stress ? "Whole screen" : "As used", (unsigned) this->frames_, secs, this->frames_ / secs, ms,
             this->worst_us_ / 1000.0f, max_fps);
    // The sensors carry the whole-screen figure only: that is the one that
    // compares two firmwares, and a quiet UI would otherwise overwrite it.
    if (stress && this->ms_sensor_ != nullptr)
      this->ms_sensor_->publish_state(ms);
    if (stress && this->fps_sensor_ != nullptr)
      this->fps_sensor_->publish_state(max_fps);
  } else if (stress) {
    ESP_LOGW(TAG, "Nothing was redrawn in %.1f s", secs);
  }
  this->frames_ = 0;
  this->total_us_ = 0;
  this->worst_us_ = 0;
  this->window_start_ = now;
}

void LvglBench::dump_config() {
  ESP_LOGCONFIG(TAG, "LVGL bench: reports every %u ms; lvgl_bench.run redraws the whole screen",
                (unsigned) this->interval_ms_);
}

}  // namespace lvgl_bench
}  // namespace esphome
