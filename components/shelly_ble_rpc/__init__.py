"""Poll Shelly Gen2 switch inputs using the existing ESPHome BLE client."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, ble_client
from esphome.const import CONF_ID, DEVICE_CLASS_CONNECTIVITY, ENTITY_CATEGORY_DIAGNOSTIC

DEPENDENCIES = ["ble_client"]
AUTO_LOAD = ["binary_sensor", "json"]
MULTI_CONF = True

shelly_ble_rpc_ns = cg.esphome_ns.namespace("shelly_ble_rpc")
ShellyBLERPC = shelly_ble_rpc_ns.class_(
    "ShellyBLERPC", cg.PollingComponent, ble_client.BLEClientNode
)
INPUT_KEYS = [f"input_{index}" for index in range(4)]

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ShellyBLERPC),
            **{cv.Optional(key): binary_sensor.binary_sensor_schema().extend({
                cv.Optional("on_delay", default="0s"): cv.positive_time_period_milliseconds,
            }) for key in INPUT_KEYS},
            cv.Optional("connected"): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_CONNECTIVITY,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional("response_timeout", default="5s"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(min=cv.TimePeriod(milliseconds=500), max=cv.TimePeriod(seconds=30)),
            ),
        }
    )
    .extend(ble_client.BLE_CLIENT_SCHEMA)
    .extend(cv.polling_component_schema("5s")),
    cv.has_at_least_one_key(*INPUT_KEYS),
    cv.only_on_esp32,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)
    cg.add(var.set_response_timeout(config["response_timeout"]))
    for index, key in enumerate(INPUT_KEYS):
        if key in config:
            sensor = await binary_sensor.new_binary_sensor(config[key])
            cg.add(var.set_input(index, sensor))
            cg.add(var.set_on_delay(index, config[key]["on_delay"]))
    if "connected" in config:
        sensor = await binary_sensor.new_binary_sensor(config["connected"])
        cg.add(var.set_connected(sensor))
