// Stand-in for ESPHome's display::Display: a panel that keeps what it is
// sent, so a test can read the glass back. draw_pixels_at() has ESPHome's
// packed-rows overload and nothing else, which is all lvgl_ortho uses.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
namespace esphome {
namespace display {
enum ColorOrder : uint8_t { COLOR_ORDER_RGB = 0, COLOR_ORDER_BGR = 1, COLOR_ORDER_GRB = 2 };
enum ColorBitness : uint8_t { COLOR_BITNESS_888 = 0, COLOR_BITNESS_565 = 1, COLOR_BITNESS_332 = 2 };
class Display {
 public:
  Display(int w, int h) : w_(w), h_(h), glass(size_t(w) * h * 2, 0) {}
  int get_native_width() { return this->w_; }
  int get_native_height() { return this->h_; }
  void draw_pixels_at(int x, int y, int w, int h, const uint8_t *ptr, ColorOrder, ColorBitness bitness, bool) {
    this->calls++;
    if (bitness != COLOR_BITNESS_565 || x < 0 || y < 0 || x + w > this->w_ || y + h > this->h_) {
      this->bad++;
      return;
    }
    for (int r = 0; r < h; r++)
      std::memcpy(&this->glass[(size_t(y + r) * this->w_ + x) * 2], ptr + size_t(r) * w * 2, size_t(w) * 2);
    this->pixels += uint64_t(w) * h;
  }
  int w_, h_;
  std::vector<uint8_t> glass;
  int calls{0}, bad{0};
  uint64_t pixels{0};
};
}  // namespace display
}  // namespace esphome
