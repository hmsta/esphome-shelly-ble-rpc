#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/cover/cover.h"
#include "esphome/components/switch/switch.h"
#include "esphome/core/component.h"

namespace esphome::shelly_ble_rpc {

class ShellyBLERPC;

class ShellyRPCSwitch : public switch_::Switch {
 public:
  void set_parent(ShellyBLERPC *parent) { this->parent_ = parent; }
  void set_channel(uint8_t channel) { this->channel_ = channel; }
 protected:
  void write_state(bool state) override;
  ShellyBLERPC *parent_{nullptr};
  uint8_t channel_{0};
};

class ShellyRPCCover : public cover::Cover {
 public:
  void set_parent(ShellyBLERPC *parent) { this->parent_ = parent; }
  void set_position_control(bool enabled) { this->position_control_ = enabled; }
  cover::CoverTraits get_traits() override;
  void report(float position, cover::CoverOperation operation, bool position_known);
 protected:
  void control(const cover::CoverCall &call) override;
  ShellyBLERPC *parent_{nullptr};
  bool position_known_{false};
  bool position_control_{false};
};

// One RPC transaction at a time, using Shelly's DATA/TX_CTL/RX_CTL GATT service.
// The BLE stack, connection lifecycle and Wi-Fi remain owned by ESPHome.
class ShellyBLERPC : public PollingComponent, public ble_client::BLEClientNode {
 public:
  void setup() override;
  void dump_config() override;
  void update() override;
  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;
  void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) override;
  void set_input(uint8_t index, binary_sensor::BinarySensor *sensor) { this->inputs_[index] = sensor; }
  void set_on_delay(uint8_t index, uint32_t delay) { this->on_delays_[index] = delay; }
  void set_connected(binary_sensor::BinarySensor *sensor) { this->connected_ = sensor; }
  void set_switch(uint8_t index, ShellyRPCSwitch *output) { this->switches_[index] = output; }
  void set_cover(ShellyRPCCover *output) { this->cover_ = output; }
  void command_switch(uint8_t index, bool state);
  void command_cover(uint8_t command, uint8_t position = 0);
  void set_response_timeout(uint32_t timeout) { this->response_timeout_ = timeout; }
  void set_pairing(bool pairing) { this->pairing_ = pairing; }
  void set_pairing_timeout(uint32_t timeout) { this->pairing_timeout_ = timeout; }

 protected:
  enum class Phase : uint8_t { IDLE, WRITE_LENGTH, WRITE_DATA, WAIT_LENGTH, READ_LENGTH, READ_DATA };
  // Metering and cover status replies are substantially larger than input replies.
  static constexpr size_t MAX_FRAME_SIZE = 2048;
  // Always fits the mandatory minimum ATT MTU (23). No dependency on MTU negotiation.
  static constexpr size_t WRITE_CHUNK_SIZE = 20;

  void start_request_();
  void start_poll_request_();
  void advance_poll_();
  void start_next_command_();
  void finish_poll_();
  void write_chunk_();
  void read_length_();
  void read_data_();
  void process_frame_();
  void fail_(const char *reason);
  void invalidate_();
  void reset_connection_();
  void start_when_ready_();
  bool write_(uint16_t handle, uint8_t *data, uint16_t length);
  bool read_(uint16_t handle);

  std::array<binary_sensor::BinarySensor *, 4> inputs_{};
  std::array<ShellyRPCSwitch *, 2> switches_{};
  ShellyRPCCover *cover_{nullptr};
  std::array<bool, 2> pending_outputs_{};
  std::array<bool, 2> command_switch_pending_{};
  std::array<bool, 2> command_switch_state_{};
  uint8_t cover_command_pending_{0};
  uint8_t cover_target_{0};
  bool poll_active_{false};
  uint8_t poll_step_{0};
  enum class RequestKind : uint8_t { INPUT, SWITCH_STATUS, COVER_STATUS, SWITCH_SET, COVER_COMMAND };
  RequestKind request_kind_{RequestKind::INPUT};
  uint8_t request_channel_{0};
  bool request_state_{false};
  uint8_t request_cover_command_{0};
  uint8_t request_cover_target_{0};
  std::array<bool, 4> pending_states_{};
  std::array<uint32_t, 4> on_delays_{};
  std::array<bool, 4> delay_pending_{};
  binary_sensor::BinarySensor *connected_{nullptr};
  std::array<uint8_t, MAX_FRAME_SIZE> frame_{};
  std::array<char, 128> request_{};
  uint32_t response_timeout_{5000};
  uint32_t pairing_timeout_{30000};
  uint32_t request_id_{0};
  size_t frame_length_{0};
  size_t frame_received_{0};
  size_t request_length_{0};
  size_t request_sent_{0};
  size_t write_pending_{0};
  uint16_t data_handle_{0};
  uint16_t tx_handle_{0};
  uint16_t rx_handle_{0};
  uint16_t conn_id_{0xFFFF};
  uint8_t input_index_{0};
  Phase phase_{Phase::IDLE};
  bool ready_{false};
  bool pairing_{false};
  bool authenticated_{false};
  bool poll_succeeded_{false};
};

}  // namespace esphome::shelly_ble_rpc
