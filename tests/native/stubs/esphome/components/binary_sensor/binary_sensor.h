#pragma once
namespace esphome::binary_sensor {
class BinarySensor {
 public:
  void publish_state(bool value) { state = value; valid = true; publishes++; }
  void invalidate_state() { valid = false; }
  bool has_state() const { return valid; }
  bool state{false};
  bool valid{false};
  int publishes{0};
};
}
#define LOG_BINARY_SENSOR(...) ((void) 0)
