// Stand-in for ESPHome's sensor: remembers what was published.
#pragma once
namespace esphome {
namespace sensor {
class Sensor {
 public:
  void publish_state(float v) { this->state = v; this->count++; }
  float state{0};
  int count{0};
};
}  // namespace sensor
}  // namespace esphome
