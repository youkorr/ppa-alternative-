// SPDX-License-Identifier: MIT
// Copyright (c) 2026 youkorr
// Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
#include "lvgl_ortho.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cstring>

#include "esp_cache.h"
#include "esp_heap_caps.h"

namespace esphome {
namespace lvgl_ortho {

static const char *const TAG = "lvgl_ortho";

// The PPA writes whole cache lines, and the P4 is built with 64- or 128-byte
// L2 lines: 128 suits both.
static constexpr size_t LINE = 128;
static constexpr uint32_t REPORT_MS = 10000;

// One display, one instance: LVGL's callbacks carry no pointer of ours, and
// the display's user data is ESPHome lvgl's.
static LvglOrtho *g_self = nullptr;

// The touch (and any pointer) devices whose read callback is wrapped, with
// the callback each had.
static constexpr int MAX_INDEVS = 4;
static lv_indev_t *g_indev[MAX_INDEVS];
static lv_indev_read_cb_t g_read[MAX_INDEVS];

// lv_area_intersect() and lv_area_is_equal() are in LVGL 9's PRIVATE headers
// (lv_area_private.h), which a component outside LVGL cannot count on.
static bool intersect(lv_area_t *res, const lv_area_t *a, const lv_area_t *b) {
  res->x1 = LV_MAX(a->x1, b->x1);
  res->y1 = LV_MAX(a->y1, b->y1);
  res->x2 = LV_MIN(a->x2, b->x2);
  res->y2 = LV_MIN(a->y2, b->y2);
  return res->x1 <= res->x2 && res->y1 <= res->y2;
}

static bool same(const lv_area_t *a, const lv_area_t *b) {
  return a->x1 == b->x1 && a->y1 == b->y1 && a->x2 == b->x2 && a->y2 == b->y2;
}

static lv_display_rotation_t lv_rotation(int esphome_degrees) {
  // ESPHome counts clockwise and LVGL counter-clockwise: ESPHome's 90 is
  // LVGL's 270. The board evidence is in README.md (the Tab5's own fork).
  switch (esphome_degrees) {
    case 90:
      return LV_DISPLAY_ROTATION_270;
    case 180:
      return LV_DISPLAY_ROTATION_180;
    case 270:
      return LV_DISPLAY_ROTATION_90;
    default:
      return LV_DISPLAY_ROTATION_0;
  }
}

void LvglOrtho::setup() {
  this->disp_ = lv_display_get_default();
  if (this->disp_ == nullptr || this->display_ == nullptr) {
    ESP_LOGE(TAG, "No LVGL display to take over");
    this->mark_failed();
    return;
  }
  this->width_ = this->display_->get_native_width();
  this->height_ = this->display_->get_native_height();
  const int32_t lw = lv_display_get_horizontal_resolution(this->disp_);
  const int32_t lh = lv_display_get_vertical_resolution(this->disp_);
  if (lw != this->width_ || lh != this->height_) {
    ESP_LOGE(TAG,
             "LVGL is %dx%d and the panel %dx%d: remove rotation: from the lvgl: block (lvgl_ortho does the turn, "
             "rotation: %d) and from the display: block",
             (int) lw, (int) lh, (int) this->width_, (int) this->height_, this->degrees_);
    this->mark_failed();
    return;
  }
  const lv_color_format_t cf = lv_display_get_color_format(this->disp_);
  if (lv_color_format_get_size(cf) != 2) {
    ESP_LOGE(TAG, "LVGL draws in %u bytes a pixel; lvgl_ortho is written for 16 bits (color_depth: 16)",
             (unsigned) lv_color_format_get_size(cf));
    this->mark_failed();
    return;
  }

  // No padding at the end of a row: the flush hands whole rows to the
  // display, whose draw_pixels_at() copies a block in one go only when its
  // rows follow each other.
  this->stride_ = (uint32_t) this->width_ * 2;
  const size_t size = ((size_t) this->stride_ * this->height_ + LINE - 1) & ~(LINE - 1);
  this->buf_ = (uint8_t *) heap_caps_aligned_alloc(LINE, size, MALLOC_CAP_SPIRAM);
  if (this->buf_ == nullptr) {
    ESP_LOGE(TAG, "No %u bytes of PSRAM for the panel-sized buffer", (unsigned) size);
    this->mark_failed();
    return;
  }
  memset(this->buf_, 0, size);

  g_self = this;
  lv_draw_ortho_init();
  lv_display_set_buffers_with_stride(this->disp_, this->buf_, nullptr, size, this->stride_,
                                     LV_DISPLAY_RENDER_MODE_DIRECT);
  lv_display_set_flush_cb(this->disp_, flush_cb_);
  lv_display_set_rotation(this->disp_, lv_rotation(this->degrees_));
  lv_display_set_matrix_rotation(this->disp_, true);
  if (this->degrees_ != 0 && !lv_display_get_matrix_rotation(this->disp_)) {
    ESP_LOGE(TAG, "LVGL refused matrix rotation (is LV_DRAW_TRANSFORM_USE_MATRIX set?)");
    this->mark_failed();
    return;
  }

  // LVGL 9 does not turn a pointer's coordinates by the display's rotation,
  // and the touchscreen reports the glass's. lv_display_rotate_point() is the
  // exact inverse of where lv_draw_ortho draws (tested in test/main.c).
  int n = 0;
  for (lv_indev_t *indev = lv_indev_get_next(nullptr); indev != nullptr && n < MAX_INDEVS;
       indev = lv_indev_get_next(indev)) {
    if (lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER)
      continue;
    lv_display_t *d = lv_indev_get_display(indev);
    if (d != nullptr && d != this->disp_)
      continue;
    g_indev[n] = indev;
    g_read[n] = lv_indev_get_read_cb(indev);
    lv_indev_set_read_cb(indev, read_cb_);
    n++;
  }

  lv_obj_invalidate(lv_display_get_screen_active(this->disp_));
  this->ready_ = true;
  ESP_LOGI(TAG, "LVGL draws the panel's %dx%d directly at ESPHome rotation %d (%d touch device(s) turned)",
           (int) this->width_, (int) this->height_, this->degrees_, n);
}

void LvglOrtho::dump_config() {
  ESP_LOGCONFIG(TAG, "LVGL ortho:");
  ESP_LOGCONFIG(TAG, "  Rotation: %d (ESPHome, clockwise)", this->degrees_);
  ESP_LOGCONFIG(TAG, "  Panel: %dx%d, buffer in PSRAM", (int) this->width_, (int) this->height_);
}

void LvglOrtho::read_cb_(lv_indev_t *indev, lv_indev_data_t *data) {
  for (int i = 0; i < MAX_INDEVS; i++) {
    if (g_indev[i] != indev)
      continue;
    if (g_read[i] != nullptr)
      g_read[i](indev, data);
    if (g_self != nullptr && g_self->disp_ != nullptr)
      lv_display_rotate_point(g_self->disp_, &data->point);
    return;
  }
}

void LvglOrtho::flush_cb_(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  (void) px_map;  // the buffer is ours and already in the panel's orientation
  if (g_self != nullptr) {
    // In direct mode LVGL hands over the area as the UI sees it.
    lv_area_t native;
    lv_draw_ortho_native_area(disp, area, &native);
    g_self->copy_native_(native);
  }
  lv_display_flush_ready(disp);
}

void LvglOrtho::copy_native_(const lv_area_t &in) {
  lv_area_t a = in;
  if (a.x1 < 0)
    a.x1 = 0;
  if (a.y1 < 0)
    a.y1 = 0;
  if (a.x2 >= this->width_)
    a.x2 = this->width_ - 1;
  if (a.y2 >= this->height_)
    a.y2 = this->height_ - 1;
  if (a.x2 < a.x1 || a.y2 < a.y1)
    return;
  const int32_t w = a.x2 - a.x1 + 1;
  const int32_t h = a.y2 - a.y1 + 1;
  const uint32_t t0 = micros();
  // The display copies by DMA, which reads PSRAM, not the CPU's cache: what
  // LVGL has just drawn must be written back first.
  if (w * 2 >= this->width_) {
    // Wide enough: the whole rows, one block, no repacking.
    uint8_t *rows = this->buf_ + (size_t) a.y1 * this->stride_;
    const size_t len = (size_t) h * this->stride_;
    esp_cache_msync(rows, len, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    this->display_->draw_pixels_at(0, a.y1, this->width_, h, rows, display::COLOR_ORDER_RGB,
                                   display::COLOR_BITNESS_565, false);
  } else {
    // Narrow: a column of a turned UI (a clock along one edge). Whole rows
    // would copy the panel's width for it, and a block with gaps between its
    // rows goes out a row at a time, each waiting for the last. So the area
    // is packed into rows of its own and goes out as one block.
    const size_t need = ((size_t) w * h * 2 + LINE - 1) & ~(LINE - 1);
    if (need > this->scratch_len_) {
      heap_caps_free(this->scratch_);
      this->scratch_ = (uint8_t *) heap_caps_aligned_alloc(LINE, need, MALLOC_CAP_SPIRAM);
      this->scratch_len_ = this->scratch_ != nullptr ? need : 0;
      if (this->scratch_ == nullptr) {
        ESP_LOGE(TAG, "No %u bytes of PSRAM to pack an area; copying whole rows", (unsigned) need);
        lv_area_t wide = a;
        wide.x1 = 0;
        wide.x2 = this->width_ - 1;
        this->copy_native_(wide);
        return;
      }
    }
    for (int32_t y = 0; y < h; y++)
      memcpy(this->scratch_ + (size_t) y * w * 2, this->buf_ + (size_t) (a.y1 + y) * this->stride_ + a.x1 * 2,
             (size_t) w * 2);
    esp_cache_msync(this->scratch_, need, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    this->display_->draw_pixels_at(a.x1, a.y1, w, h, this->scratch_, display::COLOR_ORDER_RGB,
                                   display::COLOR_BITNESS_565, false);
  }
  this->flush_us_ += micros() - t0;
  this->flush_rows_ += h;
}

bool LvglOrtho::on_screen_(lv_obj_t *obj) const {
  return lv_obj_is_visible(obj) && lv_obj_get_screen(obj) == lv_display_get_screen_active(this->disp_);
}

bool LvglOrtho::show_video(lv_obj_t *obj, const uint8_t *frame, uint16_t width, uint16_t height, bool mirror_x,
                           bool mirror_y) {
  if (!this->ready_ || this->video_failed_ || obj == nullptr || frame == nullptr || width == 0 || height == 0)
    return false;
  if (!this->on_screen_(obj)) {
    this->clear_video();
    return false;
  }
  if (!this->video_init_) {
    esp_err_t err = lv_draw_ortho_video_init(&this->video_, this->disp_);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "No PPA client for the video (%s)", esp_err_to_name(err));
      this->video_failed_ = true;
      return false;
    }
    this->video_init_ = true;
  }

  // Centre the frame in the object as lv_image does (LV_IMAGE_ALIGN_CENTER),
  // cut to the object and to the screen: the result is shown at 1:1.
  lv_area_t at;
  lv_obj_get_coords(obj, &at);
  const int32_t ow = lv_area_get_width(&at), oh = lv_area_get_height(&at);
  const int32_t fx = at.x1 + (ow - width) / 2, fy = at.y1 + (oh - height) / 2;
  lv_area_t ui = {fx, fy, fx + width - 1, fy + height - 1};
  lv_area_t screen = {0, 0, lv_display_get_horizontal_resolution(this->disp_) - 1,
                      lv_display_get_vertical_resolution(this->disp_) - 1};
  if (!intersect(&ui, &ui, &at) || !intersect(&ui, &ui, &screen)) {
    this->clear_video();
    return true;
  }
  lv_area_t crop = {ui.x1 - fx, ui.y1 - fy, ui.x2 - fx, ui.y2 - fy};
  if (this->video_obj_ != obj || !same(&ui, &this->video_ui_) || !same(&crop, &this->video_crop_)) {
    if (this->video_obj_ != nullptr)
      lv_obj_invalidate(this->video_obj_);
    lv_draw_ortho_video_place(&this->video_, &ui);
    lv_draw_ortho_video_set_crop(&this->video_, crop.x1, crop.y1, lv_area_get_width(&crop),
                                 lv_area_get_height(&crop));
    this->video_obj_ = obj;
    this->video_ui_ = ui;
    this->video_crop_ = crop;
    ESP_LOGI(TAG, "Video: %dx%d of the frame from %d,%d, at %d,%d to %d,%d on the panel", (int) lv_area_get_width(&crop),
             (int) lv_area_get_height(&crop), (int) crop.x1, (int) crop.y1, (int) this->video_.native.x1,
             (int) this->video_.native.y1, (int) this->video_.native.x2, (int) this->video_.native.y2);
  }

  const uint32_t t0 = micros();
  esp_err_t err = lv_draw_ortho_video_show(&this->video_, frame, width, height, LV_COLOR_FORMAT_RGB565, mirror_x,
                                           mirror_y);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "The PPA refused the video frame (%s); the object is LVGL's again", esp_err_to_name(err));
    this->video_failed_ = true;
    this->clear_video();
    return false;
  }
  // The PPA wrote PSRAM behind the cache. lv_draw_ortho_video_show() cleaned
  // and dropped the window's rows before, so the CPU holds nothing stale;
  // the display reads PSRAM by DMA. Now put the window on the glass.
  this->copy_native_(this->video_.native);
  this->video_us_ += micros() - t0;
  this->video_frames_++;
  return true;
}

void LvglOrtho::clear_video() {
  if (this->video_obj_ == nullptr)
    return;
  lv_draw_ortho_set_window(this->disp_, nullptr);
  lv_obj_invalidate(this->video_obj_);
  this->video_obj_ = nullptr;
  this->video_.placed = false;
  lv_area_set(&this->video_ui_, 0, 0, -1, -1);
}

void LvglOrtho::loop() {
  if (!this->ready_)
    return;
  // A window left on a page that is no longer shown would keep LVGL out of
  // that rectangle of the next page.
  if (this->video_obj_ != nullptr && !this->on_screen_(this->video_obj_))
    this->clear_video();
  const uint32_t now = millis();
  if (now - this->last_report_ >= REPORT_MS) {
    const float s = (now - this->last_report_) / 1000.0f;
    if (this->last_report_ != 0 && s > 0) {
      ESP_LOGD(TAG, "Last %.0fs: %.1f video frames/s (%.1f ms each, PPA and copy), %.0f panel rows/s copied (%.1f ms/s)",
               s, this->video_frames_ / s, this->video_frames_ ? this->video_us_ / 1000.0f / this->video_frames_ : 0.0f,
               this->flush_rows_ / s, this->flush_us_ / 1000.0f / s);
    }
    this->last_report_ = now;
    this->video_us_ = this->video_frames_ = this->flush_us_ = this->flush_rows_ = 0;
  }
}

}  // namespace lvgl_ortho
}  // namespace esphome
