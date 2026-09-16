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
    path = tmp_path / "test.yaml"
    path.write_text(yaml.safe_dump(config))
    result = subprocess.run(
        [sys.executable, "-m", "esphome", "config", str(path)],
        capture_output=True, text=True, timeout=30,
    )
    assert (result.returncode == 0) == valid, result.stdout + result.stderr
