"""Poll Shelly Gen2 switch inputs using the existing ESPHome BLE client."""

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
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
            cv.Optional("pairing", default=False): cv.boolean,
            cv.Optional("pairing_timeout", default="30s"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(min=cv.TimePeriod(seconds=5), max=cv.TimePeriod(seconds=180)),
            ),
        }
    )
    .extend(ble_client.BLE_CLIENT_SCHEMA)
    .extend(cv.polling_component_schema("5s")),
    cv.has_at_least_one_key(*INPUT_KEYS),
    cv.only_on_esp32,
)


def _validate_pairing(config):
    if config["pairing"]:
        ble = fv.full_config.get().get("esp32_ble", {})
        # These are ESPHome's stack-wide settings. Do not silently change them
        # from this per-device component, potentially affecting other clients.
        if ble.get("auth_req_mode") not in ("bond", "sc_bond"):
            raise cv.Invalid(
                "pairing: true requires esp32_ble.auth_req_mode: bond (or sc_bond)"
            )
        if ble.get("io_capability", "none") != "none":
            raise cv.Invalid(
                "Shelly Just Works pairing requires esp32_ble.io_capability: none"
            )
    return config


FINAL_VALIDATE_SCHEMA = _validate_pairing


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)
    cg.add(var.set_response_timeout(config["response_timeout"]))
    cg.add(var.set_pairing(config["pairing"]))
    cg.add(var.set_pairing_timeout(config["pairing_timeout"]))
    for index, key in enumerate(INPUT_KEYS):
        if key in config:
            sensor = await binary_sensor.new_binary_sensor(config[key])
            cg.add(var.set_input(index, sensor))
            cg.add(var.set_on_delay(index, config[key]["on_delay"]))
    if "connected" in config:
        sensor = await binary_sensor.new_binary_sensor(config["connected"])
        cg.add(var.set_connected(sensor))
