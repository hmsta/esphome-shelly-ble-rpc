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

void ShellyBLERPC::setup() {
  this->invalidate_();
  ESP_LOGI(TAG, "Waiting for Shelly BLE device %s", this->parent()->address_str());
}

void ShellyBLERPC::dump_config() {
  ESP_LOGCONFIG(TAG, "Shelly BLE RPC inputs:");
  ESP_LOGCONFIG(TAG, "  BLE address: %s", this->parent()->address_str());
  ESP_LOGCONFIG(TAG, "  Response timeout: %u ms", static_cast<unsigned>(this->response_timeout_));
  LOG_UPDATE_INTERVAL(this);
  for (auto *sensor : this->inputs_)
    LOG_BINARY_SENSOR("  ", "Input", sensor);
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
  if (this->connected_ != nullptr)
    this->connected_->publish_state(false);
  this->status_set_warning("Waiting for a complete Shelly input poll");
}

void ShellyBLERPC::reset_connection_() {
  this->cancel_timeout("rpc_deadline");
  this->cancel_timeout("rpc_next");
  this->ready_ = false;
  this->poll_succeeded_ = false;
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
  if (!this->ready_ || this->phase_ != Phase::IDLE)
    return;
  this->input_index_ = 0;
  while (this->input_index_ < this->inputs_.size() && this->inputs_[this->input_index_] == nullptr)
    this->input_index_++;
  if (this->input_index_ < this->inputs_.size())
    this->start_request_();
}

void ShellyBLERPC::start_request_() {
  // Positive signed-range IDs are supported by Shelly and unambiguous in JSON.
  this->request_id_ = (this->request_id_ % 0x7FFFFFFFU) + 1;
  int length = snprintf(this->request_.data(), this->request_.size(),
                        "{\"id\":%u,\"method\":\"Input.GetStatus\",\"params\":{\"id\":%u}}",
                        static_cast<unsigned>(this->request_id_), static_cast<unsigned>(this->input_index_));
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
                                      ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
  if (err != ESP_OK) {
    this->fail_("Unable to queue GATT write");
    return false;
  }
  return true;
}

bool ShellyBLERPC::read_(uint16_t handle) {
  auto err = esp_ble_gattc_read_char(this->parent()->get_gattc_if(), this->conn_id_, handle, ESP_GATT_AUTH_REQ_NONE);
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
  bool valid_state = false;
  int error_code = 0;
  bool parsed = json::parse_json(this->frame_.data(), this->frame_received_, [&](JsonObject root) {
    if (!root["id"].is<uint32_t>() || root["id"].as<uint32_t>() != this->request_id_)
      return true;  // Drain unrelated responses/notifications without extending the deadline.
    matching = true;
    if (root["error"].is<JsonObject>()) {
      error_code = root["error"]["code"] | -1;
      return true;
    }
    JsonObject result = root["result"];
    if (!result["id"].is<unsigned>() || result["id"].as<unsigned>() != this->input_index_ ||
        !result["state"].is<bool>())
      return true;
    this->pending_states_[this->input_index_] = result["state"].as<bool>();
    valid_state = true;
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
    ESP_LOGW(TAG, "Shelly RPC error %d for input %u", error_code, this->input_index_);
    this->fail_(error_code == 401 ? "RPC authentication required (not supported)" : "Shelly rejected Input.GetStatus");
    return;
  }
  if (!valid_state) {
    this->fail_("Missing/invalid input state; configure the Shelly input as switch, not button");
    return;
  }
  this->cancel_timeout("rpc_deadline");
  do {
    this->input_index_++;
  } while (this->input_index_ < this->inputs_.size() && this->inputs_[this->input_index_] == nullptr);
  if (this->input_index_ < this->inputs_.size()) {
    this->start_request_();
    return;
  }
  // Publish only after the entire configured input set has been read successfully.
  this->phase_ = Phase::IDLE;
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
  if (this->connected_ != nullptr)
    this->connected_->publish_state(true);
  if (!this->poll_succeeded_) {
    ESP_LOGI(TAG, "First input poll successful");
    for (size_t i = 0; i < this->inputs_.size(); i++) {
      if (this->inputs_[i] != nullptr)
        ESP_LOGI(TAG, "  Input %u: %s", static_cast<unsigned>(i), this->pending_states_[i] ? "ON" : "OFF");
    }
    this->poll_succeeded_ = true;
  }
  ESP_LOGD(TAG, "Input poll complete");
}

void ShellyBLERPC::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                       esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_DISCONNECT_EVT:
    case ESP_GATTC_CLOSE_EVT:
      // BLEClientBase already filters events to this client before node dispatch.
      this->reset_connection_();
      break;
    case ESP_GATTC_SEARCH_CMPL_EVT: {
      if (param->search_cmpl.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "GATT service discovery status: %d", param->search_cmpl.status);
        this->fail_("GATT service discovery failed");
        break;
      }
      this->reset_connection_();
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
      this->ready_ = true;
      ESP_LOGI(TAG, "Shelly RPC service ready");
      this->update();
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
          this->fail_("RPC response exceeds 512-byte limit");
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
