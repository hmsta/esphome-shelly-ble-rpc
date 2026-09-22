#pragma once
#include <optional>
namespace esphome::cover {
constexpr float COVER_OPEN = 1.0f;
constexpr float COVER_CLOSED = 0.0f;
enum CoverOperation { COVER_OPERATION_IDLE, COVER_OPERATION_OPENING, COVER_OPERATION_CLOSING };
class CoverTraits {
 public:
  void set_supports_position(bool) {}
  void set_supports_stop(bool) {}
  void set_is_assumed_state(bool) {}
};
class CoverCall {
 public:
  bool get_stop() const { return stop; }
  std::optional<float> get_position() const { return position; }
  bool stop{false};
  std::optional<float> position;
};
class Cover {
 public:
  virtual ~Cover() = default;
  virtual CoverTraits get_traits() = 0;
  void publish_state(bool = true) { valid = true; }
  float position{0.5f};
  CoverOperation current_operation{COVER_OPERATION_IDLE};
  bool valid{false};
 protected:
  virtual void control(const CoverCall &call) = 0;
};
}
#define LOG_COVER(...) ((void) 0)
