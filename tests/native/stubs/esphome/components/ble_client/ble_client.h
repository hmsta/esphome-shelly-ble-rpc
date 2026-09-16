#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

using esp_gatt_if_t = int;
constexpr int ESP_OK = 0;
constexpr int ESP_GATT_OK = 0;
constexpr int ESP_GATT_WRITE_TYPE_RSP = 1;
constexpr int ESP_GATT_AUTH_REQ_NONE = 0;
enum esp_gattc_cb_event_t {
  ESP_GATTC_DISCONNECT_EVT, ESP_GATTC_CLOSE_EVT, ESP_GATTC_SEARCH_CMPL_EVT,
  ESP_GATTC_WRITE_CHAR_EVT, ESP_GATTC_READ_CHAR_EVT
};
struct esp_ble_gattc_cb_param_t {
  struct { int status = 0; } search_cmpl;
  struct { uint16_t conn_id = 7; uint16_t handle = 0; int status = 0; } write;
  struct {
    uint16_t conn_id = 7;
    uint16_t handle = 0;
    int status = 0;
    uint8_t *value = nullptr;
    uint16_t value_len = 0;
  } read;
};
struct Operation {
  bool write;
  uint16_t handle;
  std::vector<uint8_t> data;
};
inline std::vector<Operation> operations;
inline int queue_result = ESP_OK;
inline int esp_ble_gattc_write_char(int, uint16_t, uint16_t handle, uint16_t length, uint8_t *data, int, int) {
  operations.push_back({true, handle, {data, data + length}});
  return queue_result;
}
inline int esp_ble_gattc_read_char(int, uint16_t, uint16_t handle, int) {
  operations.push_back({false, handle, {}});
  return queue_result;
}
namespace esphome::esp32_ble_tracker {
enum class ClientState { IDLE, ESTABLISHED };
struct ESPBTUUID {
  std::string value;
  static ESPBTUUID from_raw(const char *value) { return {value}; }
};
}
namespace esphome::ble_client {
struct Characteristic { uint16_t handle; };
class BLEClient {
 public:
  const char *address_str() { return "AA:BB:CC:DD:EE:FF"; }
  int get_gattc_if() { return 1; }
  uint16_t get_conn_id() { return 7; }
  void disconnect() { disconnects++; }
  Characteristic *get_characteristic(esp32_ble_tracker::ESPBTUUID, esp32_ble_tracker::ESPBTUUID uuid) {
    auto it = characteristics.find(uuid.value);
    return it == characteristics.end() ? nullptr : &it->second;
  }
  std::map<std::string, Characteristic> characteristics{
      {"5f6d4f53-5f52-5043-5f64-6174615f5f5f", {10}},
      {"5f6d4f53-5f52-5043-5f74-785f63746c5f", {11}},
      {"5f6d4f53-5f52-5043-5f72-785f63746c5f", {12}}};
  int disconnects{0};
};
class BLEClientNode {
 public:
  virtual ~BLEClientNode() = default;
  virtual void gattc_event_handler(esp_gattc_cb_event_t, esp_gatt_if_t, esp_ble_gattc_cb_param_t *) {}
  BLEClient *parent() { return parent_; }
  void set_ble_client_parent(BLEClient *parent) { parent_ = parent; }
  esp32_ble_tracker::ClientState node_state{esp32_ble_tracker::ClientState::IDLE};
 protected:
  BLEClient *parent_{nullptr};
};
}
