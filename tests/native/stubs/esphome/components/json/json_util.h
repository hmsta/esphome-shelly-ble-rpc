#pragma once
// Use the real ArduinoJson parser; only ESPHome's allocation wrapper is replaced.
#include <ArduinoJson.h>
#include <functional>
namespace esphome::json {
inline bool parse_json(const uint8_t *data, size_t length, const std::function<bool(JsonObject)> &callback) {
  JsonDocument doc;
  if (deserializeJson(doc, data, length) || !doc.is<JsonObject>())
    return false;
  return callback(doc.as<JsonObject>());
}
}
