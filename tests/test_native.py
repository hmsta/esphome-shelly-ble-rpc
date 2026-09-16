"""Exercise the actual component C++ with a fake BLE peer and real JSON parser."""

import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_native_rpc():
    # Point ARDUINOJSON_INCLUDE at ArduinoJson 7.4.3's src directory.
    include = Path(os.environ["ARDUINOJSON_INCLUDE"])
    compiler = os.environ.get("CXX", "g++")
    build = ROOT / "tests/native/build"
    build.mkdir(exist_ok=True)
    executable = build / ("test_rpc.exe" if os.name == "nt" else "test_rpc")
    command = [compiler]
    if Path(compiler).stem == "zig":
        command.append("c++")
    command += [
        "-std=c++17", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-unused-variable",
        "-I" + str(ROOT / "tests/native/stubs"),
        "-I" + str(ROOT / "components/shelly_ble_rpc"),
        "-I" + str(include),
        str(ROOT / "components/shelly_ble_rpc/shelly_ble_rpc.cpp"),
        str(ROOT / "tests/native/test_rpc.cpp"), "-o", str(executable),
    ]
    subprocess.run(command, check=True, timeout=180)
    subprocess.run([str(executable)], check=True, timeout=20)
