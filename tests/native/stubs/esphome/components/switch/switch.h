#pragma once
namespace esphome::switch_ {
class Switch {
 public:
  virtual ~Switch() = default;
  void publish_state(bool value) { state = value; valid = true; }
  bool state{false};
  bool valid{false};
 protected:
  virtual void write_state(bool state) = 0;
};
}
#define LOG_SWITCH(...) ((void) 0)
