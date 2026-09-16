# ESPHome Shelly BLE RPC

Read Shelly inputs over Bluetooth and expose them to Home Assistant as ESPHome
binary sensors. The Shelly keeps its stock firmware; the ESP32 polls its BLE
RPC service and forwards the readings through ESPHome's native API.

This is a community **external component**, not a built-in ESPHome component
or an official Shelly integration.

## Security warning: enabling BLE RPC

> [!WARNING]
> On the tested Shelly Plus i4 with firmware 1.7.5, this component uses BLE RPC
> without a password or pairing. Treat the enabled RPC service as accessible
> to anyone within Bluetooth range. This component only reads inputs, but it
> does **not** restrict other clients to those reads or prevent configuration
> changes through the Shelly's RPC service.

Your Wi-Fi password and ESPHome API encryption do not protect this separate
Bluetooth connection. Configuring the Shelly's MAC address selects the device
for our client; it does not create an access whitelist on the Shelly. Keeping
our client connected is not a security boundary either.

Shelly introduced mandatory BLE RPC pairing outside initial setup in
[firmware 2.0.0](https://shelly-api-docs.shelly.cloud/gen2/ComponentsAndServices/BLE/#ble-security-and-bonding).
The **Plus i4 is feature-frozen on 1.7.x and will not receive 2.0.0**, according
to [Shelly's firmware policy](https://shelly-api-docs.shelly.cloud/gen2/General/FirmwareUpdatePolicy/).
This component does not implement the newer pairing mechanism.

Shelly documents Wi-Fi passwords as write-only and omits them from
[`WiFi.GetConfig`](https://shelly-api-docs.shelly.cloud/gen2/ComponentsAndServices/WiFi/#configuration).
That prevents direct retrieval through that method; it is **not** a guarantee
against other RPC misuse or firmware vulnerabilities.

Disabling **Enable RPC** closes this BLE RPC access, but also stops this
component's input polling. There is no access restriction this ESPHome client
can add to the tested Shelly firmware. Use this setup only if that nearby-access
risk is acceptable. If it is not, provide reliable Wi-Fi connectivity and use
authenticated network access with BLE RPC disabled, or evaluate hardware with
protected BLE RPC and a compatible client. Consider actual radio coverage,
including areas outside your property, when assessing the risk.

## Tested hardware

**Only the Shelly Plus i4 has been tested on physical hardware.**

| Item | Tested configuration |
| --- | --- |
| Shelly | Plus i4, SNSN-0024X, Gen2 |
| Shelly firmware | 1.7.5 |
| Inputs | Four wired, persistent ON/OFF signals, configured as switches |
| ESPHome host | ESP32-C3, ESP-IDF 5.5.5 |
| ESPHome | 2026.8.2 |

The implementation uses Gen2 `Input.GetStatus`, but compatibility with other
Shelly models, firmware generations, and ESP32 variants is **unverified**.
The four inputs and Home Assistant integration have been confirmed working in
a user installation alongside existing Modbus sensors. Delay cancellation and
error recovery are covered by automated tests; long-term field reliability and
all failure scenarios have not been characterized.

## Implemented features

- Poll any subset of input IDs 0–3; at least one input is required.
- Expose readings as binary sensors through the ESPHome native API.
- Configurable polling interval and per-request timeout.
- Optional ON delay for each input, with no additional OFF delay.
- Optional diagnostic sensor indicating successful polling.
- Persistent BLE connection and reconnection through ESPHome's BLE client.
- Invalidate readings on disconnection, RPC errors, or timeouts.
- Cancel pending ON delays on communication failure and restart after recovery.
- Bounded BLE framing, chunked transfers, and response-ID validation.

No Shelly Wi-Fi connection is needed for these readings once BLE RPC is
enabled. This does not provide access to the Shelly web UI or replace every
feature of Home Assistant's native Shelly integration.

## Prepare the Shelly

1. Enable **Bluetooth** and **Enable RPC** in its web interface, then save.
2. Note the **Bluetooth MAC address** shown there; it differs from the Wi-Fi MAC.
3. Configure the inputs as **switches**, not buttons. The component needs a
   boolean `state`; null/missing states cause the poll to fail.
4. Keep the ESP32 within Bluetooth range.

Firmware updates or provisioning may change Bluetooth/RPC settings. If the
service is missing, recheck these settings. RPC authentication challenges and
newer firmware's BLE pairing requirements are not implemented.

## Install from GitHub

Merge this into your existing ESP32 ESPHome YAML. Keep your existing Wi-Fi,
API, OTA, and other device configuration. Replace the example Bluetooth MAC.
The tested host uses the ESP-IDF framework.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/hmsta/esphome-shelly-ble-rpc
      ref: main
    components: [shelly_ble_rpc]

esp32_ble_tracker:
  scan_parameters:
    active: false

ble_client:
  - id: shelly_i4_client
    mac_address: AA:BB:CC:DD:EE:FF  # Replace with the Shelly Bluetooth MAC

shelly_ble_rpc:
  id: shelly_i4
  ble_client_id: shelly_i4_client
  update_interval: 5s
  response_timeout: 5s

  input_0:
    name: "Shelly i4 Input 1"
    # on_delay: 10s
  input_1:
    name: "Shelly i4 Input 2"
    # on_delay: 10s
  input_2:
    name: "Shelly i4 Input 3"
  input_3:
    name: "Shelly i4 Input 4"

  connected:
    name: "Shelly i4 Polling Healthy"
```

`main` follows development. For reproducible deployments, replace `main` with
the full commit SHA you have tested. No release tags are assumed by this example.

These are ESPHome entities belonging to the ESP32 device. They do not reuse
the entity identities from Home Assistant's native Shelly integration.

### Local installation

Copy `components/shelly_ble_rpc` into a `components` directory beside your
device YAML, and use this source instead:

```yaml
external_components:
  - source:
      type: local
      path: components
    components: [shelly_ble_rpc]
```

With Docker, that directory must be inside the mounted ESPHome configuration
directory, normally `/config`. A standalone local-checkout example is provided
in [examples/shelly-i4-ble.yaml](examples/shelly-i4-ble.yaml). Set its Wi-Fi
credentials and Bluetooth address before flashing.

## Configuration

| Option | Default | Meaning |
| --- | --- | --- |
| `ble_client_id` | Required | ESPHome BLE client for the Shelly |
| `update_interval` | `5s` | Scheduled polling interval; overlapping polls are skipped |
| `response_timeout` | `5s` | Deadline per input request, including its transfer; range `500ms`–`30s` |
| `input_0` … `input_3` | Omitted | Binary sensor for input ID 0 … 3; at least one required |
| `input_N.on_delay` | `0s` | Delay before publishing an observed ON |
| `connected` | Omitted | Diagnostic binary sensor for polling health |

Input 0 corresponds to the Plus i4's first physical input. Each input accepts
normal ESPHome binary-sensor configuration plus `on_delay`. Multiple component
instances may be configured with separate BLE clients; multiple-device operation
has not been field tested.

### Polling and delays

The component reads configured inputs sequentially and processes their states
after the whole poll succeeds. This is not an atomic hardware snapshot. There
is an immediate poll after service discovery, followed by scheduled polling.

`on_delay: 10s` starts a timer on the first complete poll reporting ON. Repeated
ON polls do not restart it. An OFF reading cancels the timer and is published
after its complete poll, with no additional OFF delay. Detection itself is
limited by polling; short changes between polls can be missed.

At boot or after reconnection, an ON input with a delay remains **Unknown**
until the timer expires. A previously valid OFF remains OFF during a pending
ON delay. Restarting the ESP32 does not preserve a pending timer.

Use `on_delay` for this behavior instead of adding `filters: delayed_on`.
ESPHome's standard delay-filter timers are not cancelled when this component
invalidates a sensor. Other filters which retain queued values also require
care around disconnections.

### Failure behavior

When a communication failure is detected, all configured inputs lose their
valid state (normally **Unknown** in HA), all pending delays are cancelled, and
the polling diagnostic turns OFF. The inputs are not forced OFF. Local
automations should check `has_state()` rather than trusting an old boolean.

Transport failures close the BLE connection to discard partial frames.
ESPHome can then rediscover and reconnect the client. A complete successful
poll restores health; delayed ON inputs start a new timer. The diagnostic may
be ON while an input's delay is still pending. Link-loss detection is subject
to BLE supervision and RPC timeouts, not instantaneous.

## Logs and troubleshooting

At INFO level, expect:

```text
Waiting for Shelly BLE device ...
Shelly RPC service ready
First input poll successful
  Input 0: ON
```

The first successful poll logs each configured raw input state, before its ON
delay. Later polls log `Input poll complete` at DEBUG. A startup warning about
waiting for a complete poll is expected until a successful read.

- **Waiting for the device:** check range, Bluetooth settings, and Bluetooth MAC.
- **RPC service missing:** enable and save Bluetooth RPC on the Shelly.
- **Invalid input state:** check switch mode and that the input is enabled.
- **RPC error 401:** authentication is required and is not supported here.
- **`on_delay` rejected by the editor:** verify the updated `__init__.py` is in
  the component directory. A long-running builder/editor may need restarting
  to reload the Python schema. Clearing firmware build files alone does not
  reload that process.

## Limits

- Reads input states only: no relay control, power measurements, or Shelly
  configuration changes.
- No BTHome sensor decoding, Wi-Fi bridge, NAT, or web-UI proxy.
- Polling is unsuitable for pulse counting or guaranteed button-event capture.
- RPC authentication and firmware 2.x BLE pairing are not supported.
- Responses are limited to 512 bytes; larger frames fail the poll.
- BLE adds stack memory usage and shares the ESP32-C3 radio with Wi-Fi.
- A successful compile does not establish radio reliability or runtime free
  memory. Validate with your hardware and configuration.

## Development and tests

Use Python 3.12 and the pinned ESPHome version:

```sh
python -m venv .venv
# Activate the virtual environment for your shell.
python -m pip install -r requirements-dev.txt
python -m pytest tests/test_config.py -q
python -m esphome compile examples/shelly-i4-ble.yaml
```

The native tests compile the actual component against mocked ESPHome/GATT
interfaces and the real ArduinoJson 7.4.3 parser. A C++17 compiler is required:

```sh
git clone --depth 1 --branch v7.4.3 https://github.com/bblanchon/ArduinoJson.git .cache/ArduinoJson
export ARDUINOJSON_INCLUDE="$PWD/.cache/ArduinoJson/src"
export CXX=g++
python -m pytest tests/test_native.py -q
```

On PowerShell, use `$env:ARDUINOJSON_INCLUDE` and `$env:CXX` instead of `export`.
The runner also accepts a Zig executable as `CXX` and invokes `zig c++`.

Tests cover configuration validation, framing, chunked reads/writes, response
IDs, ON/OFF states, partial input sets, malformed replies, timeouts, failed
discovery, reconnects, state invalidation, and delayed-ON cancellation/recovery.
They do not emulate the full Bluetooth stack or RF conditions. CI runs these
software tests; firmware builds and physical testing are separate checks.

For compatibility reports, include the Shelly model/firmware, ESP32 variant,
ESPHome version, and relevant logs, with credentials removed.

## References

- [Shelly's BLE RPC reference client](https://github.com/ALLTERCO/Utilities/blob/master/shelly-bluetooth-rpc/shelly-bt-rpc.py)
- [Shelly Input API](https://shelly-api-docs.shelly.cloud/gen2/ComponentsAndServices/Input/)
- [ESPHome external components](https://esphome.io/components/external_components/)
- [ESPHome BLE client](https://esphome.io/components/ble_client/)

## License

MIT. See [LICENSE](LICENSE).
