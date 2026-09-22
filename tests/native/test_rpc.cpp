#include "shelly_ble_rpc.h"
#include <ArduinoJson.h>
#include <cassert>
#include <iostream>
#include <string>

using esphome::shelly_ble_rpc::ShellyBLERPC;

struct Fixture {
  esphome::ble_client::BLEClient client;
  ShellyBLERPC rpc;
  esphome::binary_sensor::BinarySensor inputs[4];
  esphome::binary_sensor::BinarySensor healthy;

  explicit Fixture(bool all = true) {
    operations.clear();
    queue_result = 0;
    rpc.set_ble_client_parent(&client);
    for (int i = 0; i < 4; i++) {
      if (all || i == 2)
        rpc.set_input(i, &inputs[i]);
    }
    rpc.set_connected(&healthy);
    rpc.setup();
  }
  void event(esp_gattc_cb_event_t type) {
    esp_ble_gattc_cb_param_t param{};
    rpc.gattc_event_handler(type, 1, &param);
  }
  void connect() {
    event(ESP_GATTC_CONNECT_EVT);
    event(ESP_GATTC_SEARCH_CMPL_EVT);
  }
  void authenticate(bool success = true, bool matching = true, bool bonded = true) {
    esp_ble_gap_cb_param_t param{};
    param.ble_security.auth_cmpl.success = success;
    param.ble_security.auth_cmpl.auth_mode = bonded ? ESP_LE_AUTH_BOND : 0;
    if (!matching)
      param.ble_security.auth_cmpl.bd_addr[0] ^= 1;
    rpc.gap_event_handler(ESP_GAP_BLE_AUTH_CMPL_EVT, &param);
  }
  void ack(uint16_t handle, int status = 0, uint16_t conn = 7) {
    esp_ble_gattc_cb_param_t param{};
    param.write.handle = handle;
    param.write.status = status;
    param.write.conn_id = conn;
    rpc.gattc_event_handler(ESP_GATTC_WRITE_CHAR_EVT, 1, &param);
  }
  void read(uint16_t handle, std::vector<uint8_t> data, int status = 0, uint16_t conn = 7) {
    esp_ble_gattc_cb_param_t param{};
    param.read.handle = handle;
    param.read.value = data.data();
    param.read.value_len = data.size();
    param.read.status = status;
    param.read.conn_id = conn;
    rpc.gattc_event_handler(ESP_GATTC_READ_CHAR_EVT, 1, &param);
  }
  void length(uint32_t n) {
    read(12, {uint8_t(n >> 24), uint8_t(n >> 16), uint8_t(n >> 8), uint8_t(n)});
  }
  uint32_t request(unsigned expected_input) {
    auto prefix = operations.back();
    assert(prefix.write && prefix.handle == 11 && prefix.data.size() == 4);
    uint32_t size = (uint32_t(prefix.data[0]) << 24) | (uint32_t(prefix.data[1]) << 16) |
                    (uint32_t(prefix.data[2]) << 8) | prefix.data[3];
    ack(11);
    std::string request;
    while (operations.back().write) {
      auto op = operations.back();
      assert(op.handle == 10 && !op.data.empty() && op.data.size() <= 20);
      request.append(op.data.begin(), op.data.end());
      ack(10);
    }
    assert(request.size() == size && operations.back().handle == 12);
    JsonDocument doc;
    assert(!deserializeJson(doc, request));
    assert(doc["method"] == "Input.GetStatus");
    assert(doc["params"]["id"].as<unsigned>() == expected_input);
    return doc["id"].as<uint32_t>();
  }
  void frame(const std::string &json, size_t chunk_size = 7) {
    length(json.size());
    for (size_t i = 0; i < json.size(); i += chunk_size) {
      assert(!operations.back().write && operations.back().handle == 10);
      auto end = std::min(i + chunk_size, json.size());
      read(10, {json.begin() + i, json.begin() + end});
    }
  }
  void response(uint32_t id, int index, bool state) {
    frame("{\"id\":" + std::to_string(id) + ",\"result\":{\"id\":" + std::to_string(index) +
          ",\"state\":" + (state ? "true" : "false") + "}}");
  }
};

void test_full_poll_and_reconnect() {
  Fixture f;
  f.connect();
  assert(f.client.pair_calls == 0 && operations.back().auth == ESP_GATT_AUTH_REQ_NONE);
  for (int i = 0; i < 4; i++) {
    uint32_t id = f.request(i);
    auto count = operations.size();
    f.rpc.update();  // Poll timer cannot overlap an outstanding request.
    assert(operations.size() == count);
    if (i == 0) {
      f.length(0);
      assert(f.rpc.timers.count("rpc_next"));
      f.rpc.fire("rpc_next");
      // Drain notifications and responses for other request IDs.
      f.frame("{\"method\":\"NotifyStatus\",\"params\":{}}");
      f.response(id + 100, 0, false);
      assert(f.rpc.timers.count("rpc_deadline"));
    }
    f.response(id, i, i % 2 == 0);
    if (i < 3)
      assert(!f.inputs[0].has_state());
  }
  assert(f.healthy.state && !f.rpc.warning && f.rpc.timers.empty());
  for (int i = 0; i < 4; i++) {
    assert(f.inputs[i].has_state());
    assert(f.inputs[i].state == (i % 2 == 0));
  }
  f.event(ESP_GATTC_DISCONNECT_EVT);
  assert(!f.healthy.state);
  for (auto &input : f.inputs)
    assert(!input.has_state());
  f.connect();
  for (int i = 0; i < 4; i++)
    f.response(f.request(i), i, false);
  assert(f.healthy.state);
}

void test_subset_and_invalid_payloads() {
  {
    Fixture f(false);
    f.connect();
    f.response(f.request(2), 2, true);
    assert(f.healthy.state && f.inputs[2].has_state());
    assert(!f.inputs[0].has_state());
  }
  const char *bad[] = {
      "{", "{\"id\":1,\"result\":{\"id\":0,\"state\":null}}",
      "{\"id\":1,\"result\":{\"id\":0,\"state\":1}}",
      "{\"id\":1,\"result\":{\"id\":3,\"state\":true}}",
      "{\"id\":1,\"error\":{\"code\":401}}", "{\"id\":1}"};
  for (auto *payload : bad) {
    Fixture f;
    f.connect();
    f.request(0);
    f.frame(payload);
    assert(f.client.disconnects == 1 && !f.healthy.state && f.rpc.timers.empty());
    assert(!f.inputs[0].has_state());
  }
}

void test_failed_refresh_invalidates_previous_states() {
  Fixture f;
  f.connect();
  for (int i = 0; i < 4; i++)
    f.response(f.request(i), i, true);
  assert(f.healthy.state);

  f.rpc.update();
  f.response(f.request(0), 0, false);
  // A partial refresh must not publish a mixture of old and new states.
  assert(f.inputs[0].has_state() && f.inputs[0].state);
  f.request(1);
  f.rpc.fire("rpc_deadline");
  assert(!f.healthy.state && f.client.disconnects == 1);
  for (auto &input : f.inputs)
    assert(!input.has_state());

  f.connect();
  for (int i = 0; i < 4; i++)
    f.response(f.request(i), i, false);
  assert(f.healthy.state && !f.rpc.warning);
  for (auto &input : f.inputs)
    assert(input.has_state() && !input.state);
}

void test_transport_failures() {
  {
    Fixture f;
    esp_ble_gattc_cb_param_t param{};
    param.search_cmpl.status = 5;
    f.event(ESP_GATTC_CONNECT_EVT);
    f.rpc.gattc_event_handler(ESP_GATTC_SEARCH_CMPL_EVT, 1, &param);
    assert(f.client.disconnects == 1 && operations.empty() && !f.healthy.state);
    f.rpc.update();
    assert(operations.empty());
    f.connect();
    for (int i = 0; i < 4; i++)
      f.response(f.request(i), i, true);
    assert(f.healthy.state && !f.rpc.warning);
  }
  for (int scenario = 0; scenario < 9; scenario++) {
    Fixture f;
    f.connect();
    f.request(0);
    switch (scenario) {
      case 0: f.length(513); break;
      case 1: f.length(0xFFFFFFFFU); break;
      case 2: f.read(12, {0, 1}); break;
      case 3: f.length(2); f.read(10, {}); break;
      case 4: f.length(2); f.read(10, {1, 2, 3}); break;
      case 5: f.read(12, {}, 5); break;
      case 6: f.length(0); f.rpc.fire("rpc_deadline"); break;
      case 7: f.length(10); f.read(10, {1}); f.rpc.fire("rpc_deadline"); break;
      case 8: f.rpc.fire("rpc_deadline"); break;
    }
    assert(f.client.disconnects == 1 && f.rpc.timers.empty());
    auto count = operations.size();
    f.read(12, {0, 0, 0, 10});  // Late callbacks after failure must not restart work.
    f.rpc.update();
    assert(operations.size() == count && !f.healthy.state);
  }
  {
    Fixture f;
    f.connect();
    f.ack(11, 5);
    assert(f.client.disconnects == 1);
  }
  {
    Fixture f;
    queue_result = 1;
    f.connect();
    assert(f.client.disconnects == 1 && f.rpc.timers.empty());
  }
  {
    Fixture f;
    f.client.characteristics.clear();
    f.connect();
    assert(f.client.disconnects == 1);
  }
  {
    Fixture f;
    f.connect();
    auto count = operations.size();
    f.ack(11, 0, 99);
    f.ack(99);
    assert(operations.size() == count);
    f.event(ESP_GATTC_CLOSE_EVT);
    assert(f.rpc.timers.empty());
  }
}

void test_pairing_gate_and_reconnect() {
  Fixture f(false);
  f.rpc.set_pairing(true);
  f.connect();
  assert(f.client.pair_calls == 1 && operations.empty());
  assert(f.rpc.timers.count("pairing_deadline") && !f.rpc.timers.count("rpc_deadline"));
  f.rpc.update();
  f.authenticate(true, false);  // Another BLE client's event must not unlock RPC.
  assert(operations.empty() && f.rpc.timers.count("pairing_deadline"));
  f.authenticate();
  assert(!f.rpc.timers.count("pairing_deadline"));
  f.response(f.request(2), 2, true);
  assert(f.healthy.state && f.inputs[2].state);
  for (const auto &op : operations)
    assert(op.auth == ESP_GATT_AUTH_REQ_NO_MITM);
  f.event(ESP_GATTC_DISCONNECT_EVT);
  operations.clear();
  f.authenticate();  // Late auth while disconnected cannot restart RPC.
  assert(operations.empty());
  f.connect();
  assert(f.client.pair_calls == 2 && operations.empty());
  f.authenticate();  // The stack reauthenticates with the saved bond.
  f.response(f.request(2), 2, false);
  assert(f.healthy.state && !f.inputs[2].state);
}

void test_pairing_failures_and_event_order() {
  for (int scenario = 0; scenario < 4; scenario++) {
    Fixture f;
    f.rpc.set_pairing(true);
    if (scenario == 0)
      f.client.pair_result = 1;
    f.connect();
    if (scenario == 1) f.authenticate(false);
    if (scenario == 2) f.rpc.fire("pairing_deadline");
    if (scenario == 3) f.authenticate(true, true, false);
    assert(f.client.disconnects == 1 && operations.empty() && f.rpc.timers.empty());
    assert(!f.inputs[0].has_state() && !f.healthy.state);
    f.authenticate();
    f.event(ESP_GATTC_SEARCH_CMPL_EVT);  // A queued completion cannot revive a failed link.
    f.rpc.update();
    assert(operations.empty() && f.client.pair_calls == 1);
  }
  {
    Fixture f(false);
    f.rpc.set_pairing(true);
    f.event(ESP_GATTC_CONNECT_EVT);
    f.authenticate();  // Peer requested security before discovery completed.
    assert(operations.empty());
    f.event(ESP_GATTC_SEARCH_CMPL_EVT);
    assert(f.client.pair_calls == 0);
    f.response(f.request(2), 2, true);
    assert(f.healthy.state);
  }
  {
    Fixture f;
    f.rpc.set_pairing(true);
    f.connect();
    f.event(ESP_GATTC_DISCONNECT_EVT);
    assert(f.rpc.timers.empty());
    f.authenticate();
    assert(operations.empty());
  }
}

int main() {
  {
    Fixture f;
    f.rpc.set_on_delay(0, 10000);
    f.rpc.set_on_delay(1, 10000);
    auto poll = [&](bool state) {
      for (int i = 0; i < 4; i++)
        f.response(f.request(i), i, state);
    };
    f.connect();
    poll(true);
    assert(!f.inputs[0].has_state() && !f.inputs[1].has_state());
    assert(f.inputs[2].state && f.inputs[3].state);
    f.rpc.fire("input_on_0");
    assert(f.inputs[0].has_state() && f.inputs[0].state);
    f.event(ESP_GATTC_DISCONNECT_EVT);
    assert(f.rpc.timers.empty() && !f.inputs[0].has_state());
    f.connect();
    poll(true);
    assert(!f.inputs[0].has_state() && f.rpc.timers.count("input_on_0"));
    f.rpc.update();
    poll(false);
    assert(f.rpc.timers.empty());
    assert(f.inputs[0].has_state() && !f.inputs[0].state);
    f.rpc.update();
    poll(true);
    f.rpc.fire("input_on_0");
    f.rpc.fire("input_on_1");
    assert(f.inputs[0].state && f.inputs[1].state);
    f.rpc.update();
    poll(false);
    assert(!f.inputs[0].state && !f.inputs[1].state);
  }
  test_full_poll_and_reconnect();
  test_subset_and_invalid_payloads();
  test_failed_refresh_invalidates_previous_states();
  test_transport_failures();
  test_pairing_gate_and_reconnect();
  test_pairing_failures_and_event_order();
  std::cout << "Shelly RPC transaction tests passed\n";
}
