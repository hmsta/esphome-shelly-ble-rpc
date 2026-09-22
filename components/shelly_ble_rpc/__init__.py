"""Poll Shelly Gen2 switch inputs using the existing ESPHome BLE client."""

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import binary_sensor, ble_client, cover, switch
from esphome.const import CONF_ID, DEVICE_CLASS_CONNECTIVITY, ENTITY_CATEGORY_DIAGNOSTIC

DEPENDENCIES = ["ble_client"]
AUTO_LOAD = ["binary_sensor", "cover", "json", "switch"]
MULTI_CONF = True

shelly_ble_rpc_ns = cg.esphome_ns.namespace("shelly_ble_rpc")
ShellyBLERPC = shelly_ble_rpc_ns.class_(
    "ShellyBLERPC", cg.PollingComponent, ble_client.BLEClientNode
)
ShellyRPCSwitch = shelly_ble_rpc_ns.class_("ShellyRPCSwitch", switch.Switch)
ShellyRPCCover = shelly_ble_rpc_ns.class_("ShellyRPCCover", cover.Cover)
INPUT_KEYS = [f"input_{index}" for index in range(4)]

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ShellyBLERPC),
            **{cv.Optional(key): binary_sensor.binary_sensor_schema().extend({
                cv.Optional("on_delay", default="0s"): cv.positive_time_period_milliseconds,
            }) for key in INPUT_KEYS},
            cv.Optional("switch_0"): switch.switch_schema(ShellyRPCSwitch),
            cv.Optional("switch_1"): switch.switch_schema(ShellyRPCSwitch),
            cv.Optional("cover_0"): cover.cover_schema(ShellyRPCCover).extend({
                cv.Optional("position_control", default=False): cv.boolean,
            }),
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
    cv.has_at_least_one_key(*INPUT_KEYS, "switch_0", "switch_1", "cover_0"),
    lambda config: _validate_mode(config),
    cv.only_on_esp32,
)


def _validate_mode(config):
    if "cover_0" in config and ("switch_0" in config or "switch_1" in config):
        raise cv.Invalid("cover_0 cannot be combined with switch_0 or switch_1; select the Shelly profile")
    return config


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
    for index in range(2):
        key = f"switch_{index}"
        if key in config:
            output = cg.new_Pvariable(config[key][CONF_ID])
            await switch.register_switch(output, config[key])
            cg.add(output.set_parent(var))
            cg.add(output.set_channel(index))
            cg.add(var.set_switch(index, output))
    if "cover_0" in config:
        output = cg.new_Pvariable(config["cover_0"][CONF_ID])
        await cover.register_cover(output, config["cover_0"])
        cg.add(output.set_parent(var))
        cg.add(output.set_position_control(config["cover_0"]["position_control"]))
        cg.add(var.set_cover(output))
    if "connected" in config:
        sensor = await binary_sensor.new_binary_sensor(config["connected"])
        cg.add(var.set_connected(sensor))
