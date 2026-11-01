// SPDX-License-Identifier: MIT
// Copyright (c) 2026 youkorr
// Part of ppa-alternative: https://github.com/youkorr/ppa-alternative-
#include "lvgl_ortho.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <cstring>

#include "esp_cache.h"
#include "lv_draw_ortho_ppa.h"
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
  // rows follow each other -- and the panel's own frame buffers have none.
  this->stride_ = (uint32_t) this->width_ * 2;

  g_self = this;
  lv_draw_ortho_init();
  if (this->ppa_) {
    const esp_err_t err = lv_draw_ortho_ppa_init();
    this->ppa_on_ = err == ESP_OK;
    if (!this->ppa_on_)
      ESP_LOGW(TAG, "No PPA client (error 0x%x): the CPU turns the tiles", (unsigned) err);
  }
  // After the PPA, so the second core gets tiles to alternate with its turns.
  if (this->cores_ > 1) {
    this->cores_on_ = lv_draw_ortho_set_cores(2);
    if (!this->cores_on_)
      ESP_LOGW(TAG, "cores: 2 refused (LVGL built without an OS, or no internal RAM for the tile): one core draws");
  }
  if (!this->use_panel_buffers_()) {
    const size_t size = ((size_t) this->stride_ * this->height_ + LINE - 1) & ~(LINE - 1);
    this->buf_ = (uint8_t *) heap_caps_aligned_alloc(LINE, size, MALLOC_CAP_SPIRAM);
    if (this->buf_ == nullptr) {
      ESP_LOGE(TAG, "No %u bytes of PSRAM for the panel-sized buffer", (unsigned) size);
      this->mark_failed();
      return;
    }
    memset(this->buf_, 0, size);
    lv_display_set_buffers_with_stride(this->disp_, this->buf_, nullptr, size, this->stride_,
                                       LV_DISPLAY_RENDER_MODE_DIRECT);
  }
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
  if (this->panel_fbs_ > 0) {
    ESP_LOGI(TAG, "...into the panel's own %d frame buffers: nothing is turned and nothing is copied", this->panel_fbs_);
  } else {
    ESP_LOGI(TAG, "...into a buffer of its own, copied to the display. For no copy at all, use youkorr/lvgl_9.5's "
                  "mipi_dsi with framebuffers: 3");
  }
}

bool LvglOrtho::use_panel_buffers_() {
#if LVGL_ORTHO_FAST_PRESENT
  // The panel's own scan-out buffers, as youkorr/lvgl_9.5's mipi_dsi
  // publishes them. LVGL renders straight into one while the panel shows
  // another, and a flush only swaps the pointer the panel reads -- in the
  // panel's orientation, which is the half Espressif's presenter refuses
  // (its DOUBLE_DIRECT mode rejects any rotation).
  const auto &fp = mipi_dsi::g_fast_present;
  if (fp.num_fbs < 2 || fp.present == nullptr || fp.fb[0] == nullptr || fp.fb[1] == nullptr)
    return false;
  if (fp.w != this->width_ || fp.h != this->height_) {
    ESP_LOGE(TAG, "The panel's frame buffers are %dx%d and the display %dx%d; copying instead", fp.w, fp.h,
             (int) this->width_, (int) this->height_);
    return false;
  }
  const size_t size = (size_t) this->stride_ * this->height_;
  lv_display_set_buffers_with_stride(this->disp_, fp.fb[0], fp.fb[1], size, this->stride_,
                                     LV_DISPLAY_RENDER_MODE_DIRECT);
  this->panel_fbs_ = 2;
  if (fp.num_fbs >= 3 && fp.fb[2] != nullptr) {
    // Three: the buffer LVGL draws into was last shown two swaps ago, so it
    // is never the one on the glass, nor the one waiting for the next frame.
    lv_draw_buf_init(&this->third_, this->width_, this->height_, lv_display_get_color_format(this->disp_),
                     this->stride_, fp.fb[2], size);
    lv_display_set_3rd_draw_buffer(this->disp_, &this->third_);
    this->panel_fbs_ = 3;
  } else {
    ESP_LOGW(TAG, "Two frame buffers: LVGL may draw into the one just swapped out before the panel has left it "
                  "(a tear for one frame). framebuffers: 3 on mipi_dsi removes it");
  }
  return true;
#else
  return false;
#endif
}

void LvglOrtho::dump_config() {
  ESP_LOGCONFIG(TAG, "LVGL ortho:");
  ESP_LOGCONFIG(TAG, "  Rotation: %d (ESPHome, clockwise)", this->degrees_);
  ESP_LOGCONFIG(TAG, "  Panel: %dx%d, buffer in PSRAM", (int) this->width_, (int) this->height_);
  // Where every widget is drawn before it is turned: internal RAM keeps the
  // drawing off the bus the frame buffer and the scan-out share.
  ESP_LOGCONFIG(TAG, "  Drawing tile: %u KiB in %s", (unsigned) (LV_DRAW_ORTHO_TILE_BYTES / 1024),
                lv_draw_ortho_get_stats()->tile_internal ? "internal RAM" : "PSRAM (internal RAM was full)");
  if (this->ppa_on_) {
    ESP_LOGCONFIG(TAG, "  Turns: the PPA, %s", lv_draw_ortho_get_stats()->tiles_in_turn >= 2
                  ? "the CPU drawing one tile while it turns the other"
                  : "the CPU waiting for each (no internal RAM for a second tile)");
  } else {
    ESP_LOGCONFIG(TAG, "  Turns: the CPU%s", this->ppa_ ? " (no PPA client)" : " (ppa: false)");
  }
  if (this->cores_on_) {
    ESP_LOGCONFIG(TAG, "  Drawing: two cores, every other tile on the second%s",
                  lv_draw_ortho_get_stats()->tiles_in_turn == 4 ? ", a pair drawn while the last is turned" : "");
  } else {
    ESP_LOGCONFIG(TAG, "  Drawing: one core%s", this->cores_ > 1 ? " (cores: 2 refused, see above)" : "");
  }
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
#if LVGL_ORTHO_FAST_PRESENT
  if (g_self != nullptr && g_self->panel_fbs_ > 0) {
    // LVGL drew into one of the panel's own buffers. Once the whole frame is
    // drawn, the panel is told to show it: a pointer swap, no pixel moves.
    if (lv_display_flush_is_last(disp)) {
      const auto &fp = mipi_dsi::g_fast_present;
      fp.present(fp.instance, px_map);
      g_self->presents_++;
    }
    lv_display_flush_ready(disp);
    return;
  }
#endif
  if (g_self != nullptr) {
    if (lv_display_flush_is_last(disp))
      g_self->presents_++;
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
  if (this->panel_fbs_ > 0) {
    // The frame went into the panel buffer LVGL draws next. Its next refresh
    // draws around the window and swaps that buffer onto the glass; the
    // object being redrawn also keeps LVGL from syncing an older frame over
    // the window from the buffer on the glass.
    lv_obj_invalidate(obj);
  } else {
    this->copy_native_(this->video_.native);
  }
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
  // The PPA stopped answering: lv_draw_ortho turns on the CPU from now on, and
  // what it had queued is lost, so the screen is drawn again. Nothing public
  // unsticks the PPA; a reset does.
  if (this->ppa_on_ && !this->ppa_lost_ && lv_draw_ortho_get_stats()->turner_failed) {
    this->ppa_lost_ = true;
    ESP_LOGE(TAG, "The PPA stopped answering for %d ms: the CPU turns from now on, until a reset. "
                  "Please report the panel and the screen that was shown",
             LV_DRAW_ORTHO_PPA_TIMEOUT_MS);
    lv_obj_invalidate(lv_display_get_screen_active(this->disp_));
  }
  const uint32_t now = millis();
  if (now - this->last_report_ >= REPORT_MS) {
    const float s = (now - this->last_report_) / 1000.0f;
    if (this->last_report_ != 0 && s > 0) {
      // INFO, not DEBUG: this line is what says, on the board, whether it is fast.
      ESP_LOGI(TAG, "Last %.0fs: %.1f video frames/s (%.1f ms each), %.1f screens/s shown, %.0f panel rows/s copied (%.1f ms/s)",
               s, this->video_frames_ / s, this->video_frames_ ? this->video_us_ / 1000.0f / this->video_frames_ : 0.0f,
               this->presents_ / s, this->flush_rows_ / s, this->flush_us_ / 1000.0f / s);
      // How much of the turning the PPA did, and how often the CPU had to
      // wait for it: near 100% and few waits is the PPA working for nothing.
      const lv_draw_ortho_stats_t *st = lv_draw_ortho_get_stats();
      if (st->bytes_turned > 0) {
        ESP_LOGI(TAG, "  turned %.0f KiB/s, %.0f%% of it by the PPA, the CPU waited %.1f times/s for it",
                 st->bytes_turned / 1024.0f / s, 100.0f * st->bytes_handed / st->bytes_turned, st->waits / s);
      }
      if (this->cores_on_ && st->tiles > 0) {
        ESP_LOGI(TAG, "  %u of %u tiles painted on the second core", (unsigned) st->tiles_other_core,
                 (unsigned) st->tiles);
      }
      lv_draw_ortho_reset_stats();
    }
    this->last_report_ = now;
    this->video_us_ = this->video_frames_ = this->flush_us_ = this->flush_rows_ = this->presents_ = 0;
  }
}

}  // namespace lvgl_ortho
}  // namespace esphome
