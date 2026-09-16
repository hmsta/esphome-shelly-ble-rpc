#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace esphome {
class PollingComponent {
 public:
  virtual ~PollingComponent() = default;
  virtual void setup() {}
  virtual void dump_config() {}
  virtual void update() {}
  void set_timeout(const char *name, uint32_t, std::function<void()> callback) { timers[name] = callback; }
  void cancel_timeout(const char *name) { timers.erase(name); }
  void fire(const char *name) {
    auto cb = timers.at(name);
    timers.erase(name);
    cb();
  }
  void status_set_warning(const char * = nullptr) { warning = true; }
  void status_clear_warning() { warning = false; }
  std::map<std::string, std::function<void()>> timers;
  bool warning{false};
};
}
#define LOG_UPDATE_INTERVAL(...) ((void) 0)
