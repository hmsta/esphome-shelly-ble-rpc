#include "shelly_ble_rpc.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "esphome/components/esp32_ble/ble_uuid.h"
#include "esphome/components/json/json_util.h"
#include "esphome/core/log.h"

namespace esphome::shelly_ble_rpc {

static const char *const TAG = "shelly_ble_rpc";
static const char *const DELAY_TIMERS[] = {"input_on_0", "input_on_1", "input_on_2", "input_on_3"};
static const char *const SERVICE_UUID = "5f6d4f53-5f52-5043-5f53-56435f49445f";
static const char *const DATA_UUID = "5f6d4f53-5f52-5043-5f64-6174615f5f5f";
static const char *const TX_UUID = "5f6d4f53-5f52-5043-5f74-785f63746c5f";
static const char *const RX_UUID = "5f6d4f53-5f52-5043-5f72-785f63746c5f";

void ShellyRPCSwitch::write_state(bool state) {
  if (this->parent_ != nullptr)
    this->parent_->command_switch(this->channel_, state);
}

cover::CoverTraits ShellyRPCCover::get_traits() {
  auto traits = cover::CoverTraits();
  traits.set_supports_position(this->position_control_);
  traits.set_supports_stop(true);
  traits.set_is_assumed_state(!this->position_control_);
  return traits;
}

void ShellyRPCCover::control(const cover::CoverCall &call) {
  if (this->parent_ == nullptr)
    return;
  if (call.get_stop()) {
    this->parent_->command_cover(3);
  } else if (call.get_position().has_value()) {
    const float position = *call.get_position();
    if (position <= cover::COVER_CLOSED)
      this->parent_->command_cover(2);
    else if (position >= cover::COVER_OPEN)
      this->parent_->command_cover(1);
    else if (this->position_control_ && this->position_known_)
      this->parent_->command_cover(4, static_cast<uint8_t>(position * 100.0f + 0.5f));
    else
      ESP_LOGW(TAG, "Cover position command ignored: position control is disabled or position is unknown");
  }
}

void ShellyRPCCover::report(float position, cover::CoverOperation operation, bool position_known) {
  this->position_known_ = position_known;
  if (position_known)
    this->position = position;
  else
    this->position = 0.5f;
  this->current_operation = operation;
  this->publish_state(false);
}

void ShellyBLERPC::setup() {
  this->invalidate_();
  ESP_LOGI(TAG, "Waiting for Shelly BLE device %s", this->parent()->address_str());
}

void ShellyBLERPC::dump_config() {
  ESP_LOGCONFIG(TAG, "Shelly BLE RPC inputs:");
  ESP_LOGCONFIG(TAG, "  BLE address: %s", this->parent()->address_str());
  ESP_LOGCONFIG(TAG, "  Response timeout: %u ms", static_cast<unsigned>(this->response_timeout_));
  ESP_LOGCONFIG(TAG, "  Pairing: %s", this->pairing_ ? "required (bonded)" : "legacy");
  if (this->pairing_)
    ESP_LOGCONFIG(TAG, "  Pairing timeout: %u ms", static_cast<unsigned>(this->pairing_timeout_));
  LOG_UPDATE_INTERVAL(this);
  for (auto *sensor : this->inputs_)
    LOG_BINARY_SENSOR("  ", "Input", sensor);
  for (auto *output : this->switches_)
    LOG_SWITCH("  ", "Relay", output);
  LOG_COVER("  ", "Cover", this->cover_);
  LOG_BINARY_SENSOR("  ", "Polling healthy", this->connected_);
}

void ShellyBLERPC::invalidate_() {
  for (size_t i = 0; i < this->inputs_.size(); i++) {
    this->cancel_timeout(DELAY_TIMERS[i]);
    this->delay_pending_[i] = false;
  }
  for (auto *sensor : this->inputs_) {
    if (sensor != nullptr)
      sensor->invalidate_state();
  }
  // ESPHome switches and covers retain their last known value. The diagnostic
  // entity below signals that those values are stale until the next poll.
  if (this->connected_ != nullptr)
    this->connected_->publish_state(false);
  this->status_set_warning("Waiting for a complete Shelly RPC poll");
}

void ShellyBLERPC::reset_connection_() {
  this->cancel_timeout("rpc_deadline");
  this->cancel_timeout("rpc_next");
  this->cancel_timeout("pairing_deadline");
  this->ready_ = false;
  this->authenticated_ = false;
  this->poll_succeeded_ = false;
  this->poll_active_ = false;
  this->poll_step_ = 0;
  this->command_switch_pending_.fill(false);
  this->cover_command_pending_ = 0;
  this->phase_ = Phase::IDLE;
  this->data_handle_ = this->tx_handle_ = this->rx_handle_ = 0;
  this->conn_id_ = 0xFFFF;
  this->frame_length_ = this->frame_received_ = 0;
  this->invalidate_();
}

void ShellyBLERPC::fail_(const char *reason) {
  ESP_LOGW(TAG, "Polling failed: %s; reconnecting", reason);
  this->reset_connection_();
  // A partial frame can leave the peer's RPC stream out of sync. Start a fresh
  // connection instead of blindly sending another request over that stream.
  this->parent()->disconnect();
}

void ShellyBLERPC::update() {
  if (!this->ready_ || this->phase_ != Phase::IDLE || this->poll_active_)
    return;
  this->poll_active_ = true;
  this->poll_step_ = 0;
  this->start_poll_request_();
}

void ShellyBLERPC::command_switch(uint8_t index, bool state) {
  if (index >= this->switches_.size() || this->switches_[index] == nullptr)
    return;
  if (!this->ready_) {
    ESP_LOGW(TAG, "Relay command ignored: Shelly BLE RPC is not ready");
    return;
  }
  this->command_switch_pending_[index] = true;
  this->command_switch_state_[index] = state;
  if (this->ready_ && this->phase_ == Phase::IDLE)
    this->start_next_command_();
}

void ShellyBLERPC::command_cover(uint8_t command, uint8_t position) {
  if (this->cover_ == nullptr || command < 1 || command > 4)
    return;
  if (!this->ready_) {
    ESP_LOGW(TAG, "Cover command ignored: Shelly BLE RPC is not ready");
    return;
  }
  this->cover_command_pending_ = command;
  this->cover_target_ = position;
  if (this->ready_ && this->phase_ == Phase::IDLE)
    this->start_next_command_();
}

void ShellyBLERPC::start_next_command_() {
  if (!this->ready_ || this->phase_ != Phase::IDLE)
    return;
  if (this->cover_command_pending_ != 0) {
    this->request_kind_ = RequestKind::COVER_COMMAND;
    this->request_channel_ = 0;
    this->request_cover_command_ = this->cover_command_pending_;
    this->request_cover_target_ = this->cover_target_;
    this->cover_command_pending_ = 0;
    this->start_request_();
    return;
  }
  for (uint8_t i = 0; i < this->switches_.size(); i++) {
    if (this->command_switch_pending_[i]) {
      this->request_kind_ = RequestKind::SWITCH_SET;
      this->request_channel_ = i;
      this->request_state_ = this->command_switch_state_[i];
      this->command_switch_pending_[i] = false;
      this->start_request_();
      return;
    }
  }
  if (this->poll_active_)
    this->start_poll_request_();
}

void ShellyBLERPC::start_poll_request_() {
  while (this->poll_step_ < 7) {
    if (this->poll_step_ < 4 && this->inputs_[this->poll_step_] != nullptr) {
      this->request_kind_ = RequestKind::INPUT;
      this->request_channel_ = this->poll_step_;
      this->input_index_ = this->poll_step_;
      this->start_request_();
      return;
    }
    if (this->poll_step_ >= 4 && this->poll_step_ < 6 && this->switches_[this->poll_step_ - 4] != nullptr) {
      this->request_kind_ = RequestKind::SWITCH_STATUS;
      this->request_channel_ = this->poll_step_ - 4;
      this->start_request_();
      return;
    }
    if (this->poll_step_ == 6 && this->cover_ != nullptr) {
      this->request_kind_ = RequestKind::COVER_STATUS;
      this->request_channel_ = 0;
      this->start_request_();
      return;
    }
    this->poll_step_++;
  }
  this->finish_poll_();
}

void ShellyBLERPC::advance_poll_() {
  this->poll_step_++;
  this->start_next_command_();
}

void ShellyBLERPC::start_request_() {
  // Positive signed-range IDs are supported by Shelly and unambiguous in JSON.
  this->request_id_ = (this->request_id_ % 0x7FFFFFFFU) + 1;
  const char *method = nullptr;
  switch (this->request_kind_) {
    case RequestKind::INPUT: method = "Input.GetStatus"; break;
    case RequestKind::SWITCH_STATUS: method = "Switch.GetStatus"; break;
    case RequestKind::COVER_STATUS: method = "Cover.GetStatus"; break;
    case RequestKind::SWITCH_SET: method = "Switch.Set"; break;
    case RequestKind::COVER_COMMAND:
      method = this->request_cover_command_ == 1 ? "Cover.Open" :
               this->request_cover_command_ == 2 ? "Cover.Close" :
               this->request_cover_command_ == 3 ? "Cover.Stop" : "Cover.GoToPosition";
      break;
  }
  int length;
  if (this->request_kind_ == RequestKind::SWITCH_SET)
    length = snprintf(this->request_.data(), this->request_.size(),
                      "{\"id\":%u,\"method\":\"%s\",\"params\":{\"id\":%u,\"on\":%s}}",
                      unsigned(this->request_id_), method, unsigned(this->request_channel_), this->request_state_ ? "true" : "false");
  else if (this->request_kind_ == RequestKind::COVER_COMMAND && this->request_cover_command_ == 4)
    length = snprintf(this->request_.data(), this->request_.size(),
                      "{\"id\":%u,\"method\":\"%s\",\"params\":{\"id\":0,\"pos\":%u}}",
                      unsigned(this->request_id_), method, unsigned(this->request_cover_target_));
  else
    length = snprintf(this->request_.data(), this->request_.size(),
                      "{\"id\":%u,\"method\":\"%s\",\"params\":{\"id\":%u}}",
                      unsigned(this->request_id_), method, unsigned(this->request_channel_));
  if (length <= 0 || static_cast<size_t>(length) >= this->request_.size()) {
    this->fail_("Request exceeds buffer");
    return;
  }
  this->request_length_ = length;
  this->request_sent_ = 0;
  this->frame_received_ = this->frame_length_ = 0;
  this->set_timeout("rpc_deadline", this->response_timeout_, [this]() { this->fail_("RPC response timeout"); });
  uint32_t size = this->request_length_;
  uint8_t prefix[4] = {static_cast<uint8_t>(size >> 24), static_cast<uint8_t>(size >> 16),
                       static_cast<uint8_t>(size >> 8), static_cast<uint8_t>(size)};
  this->phase_ = Phase::WRITE_LENGTH;
  this->write_(this->tx_handle_, prefix, sizeof(prefix));
}

bool ShellyBLERPC::write_(uint16_t handle, uint8_t *data, uint16_t length) {
  auto err = esp_ble_gattc_write_char(this->parent()->get_gattc_if(), this->conn_id_, handle, length, data,
                                      ESP_GATT_WRITE_TYPE_RSP,
                                      this->pairing_ ? ESP_GATT_AUTH_REQ_NO_MITM : ESP_GATT_AUTH_REQ_NONE);
  if (err != ESP_OK) {
    this->fail_("Unable to queue GATT write");
    return false;
  }
  return true;
}

bool ShellyBLERPC::read_(uint16_t handle) {
  auto err = esp_ble_gattc_read_char(this->parent()->get_gattc_if(), this->conn_id_, handle,
                                    this->pairing_ ? ESP_GATT_AUTH_REQ_NO_MITM : ESP_GATT_AUTH_REQ_NONE);
  if (err != ESP_OK) {
    this->fail_("Unable to queue GATT read");
    return false;
  }
  return true;
}

void ShellyBLERPC::write_chunk_() {
  this->write_pending_ = std::min(WRITE_CHUNK_SIZE, this->request_length_ - this->request_sent_);
  this->phase_ = Phase::WRITE_DATA;
  this->write_(this->data_handle_, reinterpret_cast<uint8_t *>(this->request_.data()) + this->request_sent_,
               this->write_pending_);
}

void ShellyBLERPC::read_length_() {
  this->phase_ = Phase::READ_LENGTH;
  this->read_(this->rx_handle_);
}

void ShellyBLERPC::read_data_() {
  this->phase_ = Phase::READ_DATA;
  // Shelly advances its frame cursor on each characteristic read. These are
  // successive normal reads, not ATT read-blob operations with an offset.
  this->read_(this->data_handle_);
}

void ShellyBLERPC::process_frame_() {
  bool matching = false;
  bool valid_result = false;
  int error_code = 0;
  float cover_position = 0.5f;
  bool cover_position_known = false;
  cover::CoverOperation cover_operation = cover::COVER_OPERATION_IDLE;
  bool parsed = json::parse_json(this->frame_.data(), this->frame_received_, [&](JsonObject root) {
    if (!root["id"].is<uint32_t>() || root["id"].as<uint32_t>() != this->request_id_)
      return true;  // Drain unrelated responses/notifications without extending the deadline.
    matching = true;
    if (root["error"].is<JsonObject>()) {
      error_code = root["error"]["code"] | -1;
      return true;
    }
    if (this->request_kind_ == RequestKind::COVER_COMMAND) {
      valid_result = root["result"].isNull();
      return true;
    }
    JsonObject result = root["result"];
    if (this->request_kind_ == RequestKind::SWITCH_SET) {
      valid_result = result["was_on"].is<bool>();
      return true;
    }
    if (!result["id"].is<unsigned>() || result["id"].as<unsigned>() != this->request_channel_)
      return true;
    if (this->request_kind_ == RequestKind::INPUT) {
      if (!result["state"].is<bool>())
        return true;
      this->pending_states_[this->request_channel_] = result["state"].as<bool>();
    } else if (this->request_kind_ == RequestKind::SWITCH_STATUS) {
      if (!result["output"].is<bool>())
        return true;
      this->pending_outputs_[this->request_channel_] = result["output"].as<bool>();
    } else {
      const char *state = result["state"];
      if (state == nullptr)
        return true;
      if (strcmp(state, "opening") == 0)
        cover_operation = cover::COVER_OPERATION_OPENING;
      else if (strcmp(state, "closing") == 0)
        cover_operation = cover::COVER_OPERATION_CLOSING;
      else if (strcmp(state, "open") != 0 && strcmp(state, "closed") != 0 &&
               strcmp(state, "stopped") != 0 && strcmp(state, "calibrating") != 0)
        return true;
      if (result["current_pos"].is<float>() || result["current_pos"].is<int>()) {
        float pos = result["current_pos"].as<float>();
        if (pos < 0 || pos > 100)
          return true;
        cover_position = pos / 100.0f;
        cover_position_known = true;
      } else if (strcmp(state, "open") == 0) {
        cover_position = cover::COVER_OPEN;
        cover_position_known = true;
      } else if (strcmp(state, "closed") == 0) {
        cover_position = cover::COVER_CLOSED;
        cover_position_known = true;
      }
    }
    valid_result = true;
    return true;
  });
  if (!parsed) {
    this->fail_("Malformed RPC JSON");
    return;
  }
  if (!matching) {
    this->read_length_();
    return;
  }
  if (error_code != 0) {
    ESP_LOGW(TAG, "Shelly RPC error %d for channel %u", error_code, this->request_channel_);
    this->fail_(error_code == 401 ? "RPC password authentication required (not supported by BLE bonding)" : "Shelly rejected RPC request");
    return;
  }
  if (!valid_result) {
    this->fail_("Missing or invalid Shelly RPC result; check profile and configured component IDs");
    return;
  }
  this->cancel_timeout("rpc_deadline");
  this->phase_ = Phase::IDLE;
  if (this->request_kind_ == RequestKind::SWITCH_SET || this->request_kind_ == RequestKind::COVER_COMMAND) {
    // The command reply does not establish the resulting state. Read it back.
    this->poll_active_ = true;
    this->poll_step_ = 0;
    this->start_next_command_();
    return;
  }
  if (this->request_kind_ == RequestKind::COVER_STATUS)
    this->cover_->report(cover_position, cover_operation, cover_position_known);
  this->advance_poll_();
}

void ShellyBLERPC::finish_poll_() {
  this->poll_active_ = false;
  // Publish inputs and relay states only after the configured poll completes.
  this->status_clear_warning();
  for (size_t i = 0; i < this->inputs_.size(); i++) {
    if (this->inputs_[i] != nullptr)
    {
      auto *sensor = this->inputs_[i];
      if (!this->pending_states_[i] || this->on_delays_[i] == 0) {
        this->cancel_timeout(DELAY_TIMERS[i]);
        this->delay_pending_[i] = false;
        sensor->publish_state(this->pending_states_[i]);
      } else if (!this->delay_pending_[i] && !(sensor->has_state() && sensor->state)) {
        this->delay_pending_[i] = true;
        this->set_timeout(DELAY_TIMERS[i], this->on_delays_[i], [this, i]() {
          this->delay_pending_[i] = false;
          if (this->ready_ && this->pending_states_[i])
            this->inputs_[i]->publish_state(true);
        });
      }
    }
  }
  for (size_t i = 0; i < this->switches_.size(); i++) {
    if (this->switches_[i] != nullptr)
      this->switches_[i]->publish_state(this->pending_outputs_[i]);
  }
  if (this->connected_ != nullptr)
    this->connected_->publish_state(true);
  if (!this->poll_succeeded_) {
    ESP_LOGI(TAG, "First Shelly RPC poll successful");
    for (size_t i = 0; i < this->inputs_.size(); i++) {
      if (this->inputs_[i] != nullptr)
        ESP_LOGI(TAG, "  Input %u: %s", static_cast<unsigned>(i), this->pending_states_[i] ? "ON" : "OFF");
    }
    this->poll_succeeded_ = true;
  }
  ESP_LOGD(TAG, "Input poll complete");
}

void ShellyBLERPC::start_when_ready_() {
  if (this->ready_ || this->conn_id_ == 0xFFFF || this->data_handle_ == 0 ||
      this->tx_handle_ == 0 || this->rx_handle_ == 0 || (this->pairing_ && !this->authenticated_))
    return;
  this->ready_ = true;
  ESP_LOGI(TAG, "Shelly RPC service ready%s", this->pairing_ ? " (authenticated BLE link)" : "");
  this->update();
}

void ShellyBLERPC::gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  // BLEClient forwards GAP events to every node, including events for other
  // peers. Unlike GATTC events, they must be filtered here by remote address.
  if (!this->pairing_ || this->conn_id_ == 0xFFFF || event != ESP_GAP_BLE_AUTH_CMPL_EVT ||
      !this->parent()->check_addr(param->ble_security.auth_cmpl.bd_addr))
    return;
  const auto &auth = param->ble_security.auth_cmpl;
  if (!auth.success) {
    ESP_LOGW(TAG, "BLE authentication failed, reason=0x%02X", unsigned(auth.fail_reason));
    this->fail_("BLE pairing failed; check Shelly pairing window and saved bonds");
    return;
  }
  if ((auth.auth_mode & ESP_LE_AUTH_BOND) == 0) {
    this->fail_("BLE peer did not negotiate bonding; refusing unbonded RPC");
    return;
  }
  this->cancel_timeout("pairing_deadline");
  this->authenticated_ = true;
  ESP_LOGI(TAG, "Shelly BLE authentication successful (bonding enabled)");
  // Authentication may complete before service discovery; both are required.
  this->start_when_ready_();
}

void ShellyBLERPC::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                       esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_CONNECT_EVT:
      this->reset_connection_();
      this->conn_id_ = this->parent()->get_conn_id();
      break;
    case ESP_GATTC_DISCONNECT_EVT:
    case ESP_GATTC_CLOSE_EVT:
      // BLEClientBase already filters events to this client before node dispatch.
      this->reset_connection_();
      break;
    case ESP_GATTC_SEARCH_CMPL_EVT: {
      // Ignore queued discovery completion after a failure/disconnect.
      if (this->conn_id_ == 0xFFFF)
        break;
      if (param->search_cmpl.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "GATT service discovery status: %d", param->search_cmpl.status);
        this->fail_("GATT service discovery failed");
        break;
      }
      // Keep authentication received after CONNECT but before discovery.
      this->conn_id_ = this->parent()->get_conn_id();
      auto service = esp32_ble_tracker::ESPBTUUID::from_raw(SERVICE_UUID);
      auto *data = this->parent()->get_characteristic(service, esp32_ble_tracker::ESPBTUUID::from_raw(DATA_UUID));
      auto *tx = this->parent()->get_characteristic(service, esp32_ble_tracker::ESPBTUUID::from_raw(TX_UUID));
      auto *rx = this->parent()->get_characteristic(service, esp32_ble_tracker::ESPBTUUID::from_raw(RX_UUID));
      if (data == nullptr || tx == nullptr || rx == nullptr) {
        this->fail_("Shelly RPC service missing; verify Bluetooth and Enable RPC are saved");
        break;
      }
      this->data_handle_ = data->handle;
      this->tx_handle_ = tx->handle;
      this->rx_handle_ = rx->handle;
      // Only handles are retained; parent may now free its discovery cache.
      this->node_state = esp32_ble_tracker::ClientState::ESTABLISHED;
      if (this->pairing_ && !this->authenticated_) {
        this->status_set_warning("Waiting for Shelly BLE pairing/authentication");
        this->set_timeout("pairing_deadline", this->pairing_timeout_, [this]() {
          this->fail_("BLE pairing timed out; enable pairing on the Shelly for a new bond");
        });
        ESP_LOGI(TAG, "Requesting Shelly BLE encryption/bonding");
        // ESPHome/ESP-IDF owns security and persistent keys. On reconnection
        // this reuses an existing bond; no RPC is sent until AUTH_CMPL succeeds.
        const auto err = this->parent()->pair();
        if (err != ESP_OK) {
          ESP_LOGW(TAG, "Unable to request BLE pairing: %d", int(err));
          this->fail_("Unable to start BLE pairing");
          break;
        }
      }
      this->start_when_ready_();
      break;
    }
    case ESP_GATTC_WRITE_CHAR_EVT: {
      if (!this->ready_ || param->write.conn_id != this->conn_id_)
        break;
      bool length_ack = this->phase_ == Phase::WRITE_LENGTH && param->write.handle == this->tx_handle_;
      bool data_ack = this->phase_ == Phase::WRITE_DATA && param->write.handle == this->data_handle_;
      if (!length_ack && !data_ack)
        break;
      if (param->write.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "GATT write status: %d", param->write.status);
        this->fail_("GATT write rejected");
        break;
      }
      if (data_ack)
        this->request_sent_ += this->write_pending_;
      if (this->request_sent_ < this->request_length_)
        this->write_chunk_();
      else
        this->read_length_();
      break;
    }
    case ESP_GATTC_READ_CHAR_EVT: {
      if (!this->ready_ || param->read.conn_id != this->conn_id_)
        break;
      bool length_read = this->phase_ == Phase::READ_LENGTH && param->read.handle == this->rx_handle_;
      bool data_read = this->phase_ == Phase::READ_DATA && param->read.handle == this->data_handle_;
      if (!length_read && !data_read)
        break;
      if (param->read.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "GATT read status: %d", param->read.status);
        this->fail_("GATT read rejected");
        break;
      }
      const auto *value = param->read.value;
      size_t length = param->read.value_len;
      if (length_read) {
        if (length != 4) {
          this->fail_("RPC length must contain four bytes");
          break;
        }
        uint32_t size = (static_cast<uint32_t>(value[0]) << 24) | (static_cast<uint32_t>(value[1]) << 16) |
                        (static_cast<uint32_t>(value[2]) << 8) | value[3];
        if (size == 0) {
          this->phase_ = Phase::WAIT_LENGTH;
          this->set_timeout("rpc_next", 100, [this]() {
            if (this->ready_ && this->phase_ == Phase::WAIT_LENGTH)
              this->read_length_();
          });
        } else if (size > MAX_FRAME_SIZE) {
          this->fail_("RPC response exceeds 2048-byte limit");
        } else {
          this->frame_length_ = size;
          this->frame_received_ = 0;
          this->read_data_();
        }
      } else {
        if (length == 0 || length > this->frame_length_ - this->frame_received_) {
          this->fail_("Empty or oversized RPC data chunk");
          break;
        }
        memcpy(this->frame_.data() + this->frame_received_, value, length);
        this->frame_received_ += length;
        if (this->frame_received_ == this->frame_length_)
          this->process_frame_();
        else
          this->read_data_();
      }
      break;
    }
    default:
      break;
  }
}

}  // namespace esphome::shelly_ble_rpc
