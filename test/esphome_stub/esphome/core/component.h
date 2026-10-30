// Stand-in for ESPHome's Component, for tools that run lvgl_ortho on a PC:
// only what lvgl_ortho.cpp calls.
#pragma once
namespace esphome {
namespace setup_priority {
static constexpr float LATE = -100.0f;
}
class Component {
 public:
  virtual ~Component() = default;
  virtual void setup() {}
  virtual void loop() {}
  virtual void dump_config() {}
  virtual float get_setup_priority() const { return 0.0f; }
  void mark_failed() { this->failed_ = true; }
  bool is_failed() const { return this->failed_; }

 protected:
  bool failed_{false};
};
}  // namespace esphome
