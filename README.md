# ESPHome Shelly BLE RPC

Use an ESP32 to read Shelly inputs and control Shelly relays or a cover over
Bluetooth. The Shelly keeps its stock firmware, and ESPHome exposes the
configured entities to Home Assistant through its native API.

This is a community [ESPHome external component](https://esphome.io/components/external_components/).
It is neither a built-in ESPHome component nor an official Shelly integration.

## Supported functions

| Shelly device / profile | Available entities | Configuration |
| --- | --- | --- |
| Plus i4 | Up to four binary inputs | `input_0` through `input_3` |
| 1PM switch profile (tested on 1PM Gen3) | One relay | `switch_0` |
| 2PM switch profile | Two independent relays | `switch_0`, `switch_1` |
| 2PM cover profile | One cover with open, close, and stop | `cover_0` |

Position control is also available for a **calibrated** 2PM cover when
`cover_0.position_control: true`. Switch and cover profiles are alternatives:
a 2PM in cover profile cannot expose two independent relay switches. Inputs
can be configured alongside the outputs that the device actually provides.
Two separate 1PMs require two BLE clients and two component instances.

These are supported RPC functions, not a claim that every model or firmware
combination has been tested. Physical testing covers input polling on a
**Shelly Plus i4, firmware 1.7.5**, and bonded pairing and relay control on a
**Shelly 1PM Gen3, firmware 2.0.0**, using an ESP32-C3 running ESPHome 2026.8.2.
The 1PM Gen3 test used `switch_0`, `pairing: true`, and the public GitHub
component source. The 2PM switch and cover paths have native tests and
successful firmware builds, but still need field testing on Shelly hardware.
Shelly 2.5 (Generation 1) does not provide the BLE RPC service used here.

> [!WARNING]
> On legacy firmware such as Plus i4 1.7.5, enabling BLE RPC makes the
> Shelly's RPC service available to nearby Bluetooth clients without pairing.
> The configured MAC address identifies the target for this ESP32; it is not
> an access list on the Shelly. ESPHome API encryption also does not protect
> this separate Bluetooth service. See [BLE security and pairing](#ble-security-and-pairing)
> before enabling it.

## Quick start

You need an ESP32 supported by ESPHome, a Shelly with BLE RPC, and ESPHome's
`esp32_ble_tracker` and `ble_client` components. Use the Shelly's Bluetooth
MAC address below, not the ESP32's address.

1. In the Shelly web interface, enable **Bluetooth** and **RPC over BLE**.
   Select the desired switch or cover profile on the Shelly itself. For
   position control, calibrate the cover before enabling the slider in ESPHome.
2. Add the shared ESPHome configuration below to an existing ESP32 YAML file.
   Replace the MAC address and set the `ble_client` ID if it conflicts with
   another client in your file.
3. Add **one** of the `shelly_ble_rpc` configurations that follows. Flash
   the ESP32, then inspect its logs and entities in Home Assistant.

The snippets use the default unpaired mode. If your Shelly runs firmware
2.0.0 or newer, add the [bonded-mode settings](#ble-security-and-pairing)
before flashing. The complete 2PM examples use bonded mode and a local
component checkout; adapt their source and security settings to your setup.

```yaml
external_components:
  - source: github://hmsta/esphome-shelly-ble-rpc@main
    components: [shelly_ble_rpc]

esp32_ble_tracker:

ble_client:
  - mac_address: "AA:BB:CC:DD:EE:FF"
    id: shelly_ble
```

For reproducible installs, replace `@main` with a commit SHA. For local
development, set the external component source to
`{type: local, path: /path/to/esphome-shelly-ble-rpc/components}`.

### Plus i4: inputs

```yaml
shelly_ble_rpc:
  - id: shelly_i4
    ble_client_id: shelly_ble
    connected:
      name: "Shelly i4 connected"
    input_0:
      name: "Shelly i4 input 0"
    input_1:
      name: "Shelly i4 input 1"
    input_2:
      name: "Shelly i4 input 2"
    input_3:
      name: "Shelly i4 input 3"
```

See the [complete i4 example](examples/shelly-i4-ble.yaml).

### 2PM switch profile: two relays

```yaml
shelly_ble_rpc:
  - id: shelly_2pm
    ble_client_id: shelly_ble
    connected:
      name: "Shelly 2PM connected"
    switch_0:
      name: "Shelly 2PM relay 0"
    switch_1:
      name: "Shelly 2PM relay 1"
```

For a 1PM, keep `switch_0` and remove `switch_1`. This has been tested with a
bonded Shelly 1PM Gen3 running firmware 2.0.0. The 2PM must be in **switch
profile**. See the [complete 2PM switch example](examples/shelly-2pm-switch-ble.yaml).

### 2PM cover profile

```yaml
shelly_ble_rpc:
  - id: shelly_cover
    ble_client_id: shelly_ble
    connected:
      name: "Shelly cover connected"
    cover_0:
      name: "Shelly cover"
      position_control: true
```

Use `position_control: false` (the default) for open, close, and stop
without a calibrated position. The 2PM must be in **cover profile**. Do not
configure `switch_0` or `switch_1` in the same component instance.
See the [complete cover example](examples/shelly-2pm-cover-ble.yaml).

## BLE security and pairing

**Legacy firmware:** Leave `pairing` at its default `false`. On the tested
Plus i4 1.7.5, BLE RPC works without bonding, and any nearby BLE RPC client
may be able to access the Shelly's RPC methods. Shelly's
[firmware policy](https://shelly-api-docs.shelly.cloud/gen2/General/FirmwareUpdatePolicy/)
lists Plus i4 and Plus 1PM as feature-frozen on the 1.7.x line.

**Firmware 2.0.0 and newer:** Shelly requires a bonded BLE client outside
its initial setup window. Set `pairing: true` in your chosen
`shelly_ble_rpc` block and configure ESPHome's BLE security:

```yaml
esp32_ble:
  io_capability: none
  auth_req_mode: bond

ble_client:
  - mac_address: "AA:BB:CC:DD:EE:FF"
    id: shelly_ble

shelly_ble_rpc:
  - id: shelly_device
    ble_client_id: shelly_ble
    pairing: true
    pairing_timeout: 30s
    switch_0:
      name: "Shelly relay"
```

Merge this with your existing YAML: define `ble_client` and
`shelly_ble_rpc` only once for each device. Flash the ESP32 first. Then
open a temporary pairing window on the **Shelly** using its web interface
or its HTTP RPC endpoint:

```text
http://SHELLY_IP/rpc/BLE.StartPairing
```

Watch the ESPHome logs for pairing and a successful RPC status read. Close
the window with `BLE.StopPairing`, and use `BLE.ListPairedDevices` on the
Shelly to inspect stored bonds. If the Shelly web API has HTTP authentication,
authenticate that HTTP request separately; it does not provide BLE RPC
authentication. A stale bond on either side may require removing the old
pairing and pairing again.

Shelly documents [BLE security and bonding](https://shelly-api-docs.shelly.cloud/gen2/ComponentsAndServices/BLE/),
including its “Just Works” pairing model. Pairing encrypts the BLE link but
does not verify the device's identity with a passkey. This project's bonded
mode has been tested on a Shelly 1PM Gen3 running firmware 2.0.0; other bonded
device profiles still need hardware testing.
See the [complete bonded example](examples/shelly-bonded-ble.yaml).

## Configuration

At least one `input_N`, `switch_N`, or `cover_0` entity is required.
`connected` can be added alongside it. IDs must match the Shelly's actual
RPC components and profile.

| Option | Default | Description |
| --- | --- | --- |
| `ble_client_id` | Required | ID of the ESPHome `ble_client` targeting the Shelly. |
| `input_0` … `input_3` | Omitted | Binary sensors for Shelly input IDs 0–3. Each accepts ESPHome binary sensor options plus `on_delay`. |
| `input_N.on_delay` | `0s` | Delay before a polled ON state is reported. Brief activity between polls may be missed. |
| `switch_0`, `switch_1` | Omitted | ESPHome switches for relay IDs 0 and 1. Configure only relays the device provides. |
| `cover_0` | Omitted | ESPHome cover for cover ID 0; cannot be combined with relay switches in the same instance. |
| `cover_0.position_control` | `false` | Enables position commands and reporting for a calibrated cover. |
| `connected` | Omitted | Binary sensor reporting BLE/RPC availability. |
| `update_interval` | `5s` | Polling interval for configured inputs and output status. |
| `response_timeout` | `5s` | RPC reply timeout, from `500ms` to `30s`. |
| `pairing` | `false` | Enable ESPHome BLE bonding for firmware that requires it. |
| `pairing_timeout` | `30s` | Pairing wait, from `5s` to `180s`. |

Normal ESPHome entity options such as `name` and `entity_category` remain
available where appropriate. See [ESPHome binary sensors](https://esphome.io/components/binary_sensor/),
[switches](https://esphome.io/components/switch/), and
[covers](https://esphome.io/components/cover/).

## How it behaves

- The ESP32 polls Shelly status over BLE RPC; input changes that occur
  entirely between polls can be missed. Shorten `update_interval` if needed,
  while accounting for BLE traffic and device responsiveness.
- Relay commands use `Switch.Set`; cover commands use the Shelly cover RPC
  methods. Commands and polls share one serialized RPC channel. A timeout
  does not automatically replay a potentially completed command.
- Output state is read back from the Shelly. After a disconnect, the last
  displayed relay or cover state can be stale until a successful read.
  The `connected` sensor drops and input states are invalidated.
- Position commands require a calibrated cover. Cover tilt/slat control and
  power or energy metering are not exposed by this component.
- RPC messages larger than 2048 bytes are unsupported. A Shelly with a
  particularly large status response may exceed that limit.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| No BLE connection | Verify Bluetooth and RPC over BLE are enabled on the Shelly, its Bluetooth MAC is correct, and the ESP32 is in range. |
| Connects but RPC is rejected | Check whether Shelly firmware requires bonding. For 2.x, use `pairing: true`, `esp32_ble.auth_req_mode: bond`, and a Shelly pairing window. |
| Only one relay appears | A 1PM provides one relay; a 2PM must be in switch profile and have both `switch_0` and `switch_1` configured. |
| Cover commands fail or no position slider | Use the Shelly's cover profile. Calibrate it before setting `position_control: true`. |
| Input events are missed | Inputs are polled. Lower `update_interval` for shorter events, within what your BLE link handles reliably. |
| State remains visible after disconnect | Check the `connected` sensor; output entities may show their last known state until the Shelly responds again. |

For diagnosis, enable `logger:` and inspect ESPHome's BLE client and
`shelly_ble_rpc` messages. Include your Shelly model, firmware, profile,
ESPHome version, relevant YAML, and redacted logs when reporting an issue.

## Development

Native tests cover RPC framing and parsing, input polling, switch and cover
commands, pairing state, and configuration validation. From the repository
root, install `requirements-dev.txt`. The native C++ test also needs a
compiler with C++17 standard library support (for example, GCC 7 or newer)
and ArduinoJson 7.4.3. Set `ARDUINOJSON_INCLUDE` to the ArduinoJson
`src` directory (and `CXX` if your compiler is not `g++`) before running
pytest:

```sh
python -m pytest -q
esphome config examples/shelly-i4-ble.yaml
esphome config examples/shelly-2pm-switch-ble.yaml
esphome config examples/shelly-2pm-cover-ble.yaml
```

ESPHome firmware builds for the 2PM switch and cover examples have also
succeeded. Physical testing currently covers the Plus i4 inputs and bonded
1PM Gen3 relay control; the 2PM profiles still need hardware validation.

## References

- [Shelly BLE component and bonding](https://shelly-api-docs.shelly.cloud/gen2/ComponentsAndServices/BLE/)
- [Shelly Plus 1PM](https://shelly-api-docs.shelly.cloud/gen2/Devices/Gen2/ShellyPlus1PM/)
- [Shelly 2PM Gen3](https://shelly-api-docs.shelly.cloud/gen2/Devices/Gen3/Shelly2PMG3/)
- [Shelly 2PM Gen4](https://shelly-api-docs.shelly.cloud/gen2/Devices/Gen4/Shelly2PMG4/)
- [ESPHome external components](https://esphome.io/components/external_components/)

## License

See [LICENSE](LICENSE).
