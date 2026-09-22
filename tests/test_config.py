"""Validate real ESPHome schema and generated configuration, without hardware."""

from copy import deepcopy
from pathlib import Path
import subprocess
import sys

import pytest
import yaml

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize(
    "change,valid",
    [
        ("normal", True),
        ("delays", True),
        ("subset", True),
        ("no_inputs", False),
        ("timeout_too_short", False),
        ("timeout_too_long", False),
        ("unknown_input", False),
        ("legacy_explicit", True),
        ("paired", True),
        ("paired_sc", True),
        ("paired_missing_bond", False),
        ("paired_no_bond", False),
        ("paired_mitm", False),
        ("paired_wrong_io", False),
        ("pairing_timeout_short", False),
        ("pairing_timeout_long", False),
        ("mixed", True),
        ("two_switches", True),
        ("one_switch_only", True),
        ("cover_only", True),
        ("cover_and_switch", False),
        ("unknown_switch", False),
    ],
)
def test_config(tmp_path, change, valid):
    config = yaml.safe_load((ROOT / "examples/shelly-i4-ble.yaml").read_text())
    config = deepcopy(config)
    config["external_components"][0]["source"]["path"] = str(ROOT / "components")
    rpc = config["shelly_ble_rpc"]
    if change == "delays":
        rpc["input_0"]["on_delay"] = "10s"
        rpc["input_1"]["on_delay"] = "10s"
    if change in ("subset", "no_inputs"):
        for index in range(4):
            if change == "no_inputs" or index != 2:
                del rpc[f"input_{index}"]
    if change == "timeout_too_short":
        rpc["response_timeout"] = "100ms"
    if change == "timeout_too_long":
        rpc["response_timeout"] = "31s"
    if change == "unknown_input":
        rpc["input_4"] = {"name": "Invalid input"}
    if change == "legacy_explicit":
        rpc["pairing"] = False
    if change.startswith("paired") or change.startswith("pairing_timeout") or change == "mixed":
        rpc["pairing"] = True
        config["esp32_ble"] = {"auth_req_mode": "bond", "io_capability": "none"}
    if change == "paired_sc":
        config["esp32_ble"]["auth_req_mode"] = "sc_bond"
    if change == "paired_missing_bond":
        del config["esp32_ble"]
    if change == "paired_no_bond":
        config["esp32_ble"]["auth_req_mode"] = "no_bond"
    if change == "paired_mitm":
        config["esp32_ble"]["auth_req_mode"] = "bond_mitm"
    if change == "paired_wrong_io":
        config["esp32_ble"]["io_capability"] = "keyboard_only"
    if change == "pairing_timeout_short":
        rpc["pairing_timeout"] = "4s"
    if change == "pairing_timeout_long":
        rpc["pairing_timeout"] = "181s"
    if change == "mixed":
        legacy = deepcopy(rpc)
        legacy.update(id="legacy_rpc", ble_client_id="legacy_client", pairing=False)
        for key in ["connected", *(f"input_{i}" for i in range(4))]:
            legacy[key]["name"] = "Legacy " + legacy[key]["name"]
        config["ble_client"].append({"id": "legacy_client", "mac_address": "AA:BB:CC:DD:EE:FF"})
        config["shelly_ble_rpc"] = [rpc, legacy]
    if change in ("two_switches", "one_switch_only", "cover_only", "cover_and_switch", "unknown_switch"):
        for index in range(4):
            del rpc[f"input_{index}"]
        if change in ("two_switches", "one_switch_only", "cover_and_switch"):
            rpc["switch_0"] = {"name": "Relay 1"}
        if change == "two_switches":
            rpc["switch_1"] = {"name": "Relay 2"}
        if change in ("cover_only", "cover_and_switch"):
            rpc["cover_0"] = {"name": "Blind", "position_control": True}
        if change == "unknown_switch":
            rpc["switch_2"] = {"name": "Invalid relay"}
    path = tmp_path / "test.yaml"
    path.write_text(yaml.safe_dump(config))
    result = subprocess.run(
        [sys.executable, "-m", "esphome", "config", str(path)],
        capture_output=True, text=True, timeout=30,
    )
    assert (result.returncode == 0) == valid, result.stdout + result.stderr
