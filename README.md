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
Optional bonded mode is implemented for newer firmware; it has not yet been
field tested. The default legacy mode remains unchanged. See [BLE pairing](#ble-pairing-firmware-200).

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
- Optional BLE bonding, authentication gate, and a separate pairing timeout.
- Legacy mode remains the default; bonded mode never falls back to unpaired RPC.
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
service is missing, recheck these settings. RPC password authentication challenges
remain unsupported; BLE bonding is handled separately as described below.

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
| `pairing` | `false` | Require bonded BLE authentication before input RPC; leave disabled for legacy devices |
| `pairing_timeout` | `30s` | Authentication deadline after discovery; range `5s`–`180s` |
| `response_timeout` | `5s` | Deadline per input request, including its transfer; range `500ms`–`30s` |
| `input_0` … `input_3` | Omitted | Binary sensor for input ID 0 … 3; at least one required |
| `input_N.on_delay` | `0s` | Delay before publishing an observed ON |
| `connected` | Omitted | Diagnostic binary sensor for polling health |

Input 0 corresponds to the Plus i4's first physical input. Each input accepts
normal ESPHome binary-sensor configuration plus `on_delay`. Multiple component
instances may be configured with separate BLE clients; multiple-device operation
has not been field tested.

### BLE pairing (firmware 2.0.0)

Existing Plus i4 / firmware 1.7.x configurations need no changes. `pairing`
defaults to `false`, retaining the existing RPC connection behavior.

For newer devices that require pairing, **you open a web address to allow
pairing for three minutes. The ESP32 then pairs with the Shelly automatically.**
You do not need to write a script or pair your computer/phone over Bluetooth.

The steps below are for a Shelly whose firmware supports BLE pairing, such as
firmware 2.0.0. The new bonded mode still needs field testing; only legacy mode
on a Plus i4 with firmware 1.7.5 has been tested on physical hardware.

#### Device support at a glance

| Device | BLE RPC | Firmware 2.0+ secure pairing |
| --- | ---: | ---: |
| **Shelly Plus i4** | Yes | **No** |
| **Shelly Plus 1PM** | Yes | **No** |
| **Shelly 1PM Gen3** | Yes | **Yes** |
| **Shelly 2PM Gen3** | Yes | **Yes** |
| **Shelly 2PM Gen4** | Yes | **Yes** |
| **Shelly 2.5 (Gen1)** | **No** | **No** |

**Plus i4 and Plus 1PM:** use legacy mode (`pairing: false`, or omit it).
They remain on the 1.7.x feature branch. **Shelly 2.5:** cannot use this BLE
component. For the listed Gen3/Gen4 devices running firmware 2.0+, use the
pairing instructions below.

These are device/firmware capabilities. Only the Plus i4 has been field tested
with this component; bonded mode and the other models still need field testing.

Sources: [Shelly firmware policy](https://shelly-api-docs.shelly.cloud/gen2/General/FirmwareUpdatePolicy/),
[BLE RPC and bonding](https://shelly-api-docs.shelly.cloud/gen2/ComponentsAndServices/BLE/),
and [Gen1 API, including Shelly 2.5](https://shelly-api-docs.shelly.cloud/gen1/).
Checked September 22, 2026.

#### 1. Find the Shelly's web address and Bluetooth address

Open the **Shelly's own web interface** in your browser. Your computer needs
network access to the Shelly for this one-time setup.

Write down the address from the browser, for example `http://192.168.10.80`.
That IP is only an example: use the address of the Shelly you are pairing,
not the ESP32's IP or the ESPHome dashboard address.

In the Shelly's Bluetooth settings, enable **RPC** (sometimes labelled
**Enable RPC**) and save. If there is a separate Bluetooth enable checkbox,
enable that too. Copy the **Bluetooth MAC address** from those settings into
the existing `ble_client.mac_address` entry in your ESPHome YAML. This is
different from the Shelly's Wi-Fi MAC address.

#### 2. Enable pairing in ESPHome and install the updated firmware

Use the updated version of this external component. For a local installation,
copy the updated `components/shelly_ble_rpc` folder beside your ESPHome YAML.

Add this top-level section to your YAML. If you already have an `esp32_ble:`
section, add these two settings to it instead of creating a second section:

```yaml
esp32_ble:
  io_capability: none
  auth_req_mode: bond
```

Inside your **existing** `shelly_ble_rpc:` section, add these two settings:

```yaml
shelly_ble_rpc:
  pairing: true
  pairing_timeout: 30s
  # Keep your existing ble_client_id, input_0, input_1, etc. here too.
```

This second snippet shows only the new settings; it is not a replacement for
your whole `shelly_ble_rpc:` section. Keep the existing input names and delays.

##### Complete Shelly configuration with pairing enabled

Merge this block into your existing ESPHome YAML. Keep your existing ESP32,
Wi-Fi, API, OTA, and other sensor settings. If any of these sections already
exist, update them instead of creating duplicate sections.

Copy the updated `components/shelly_ble_rpc` folder beside your device YAML,
and replace `AA:BB:CC:DD:EE:FF` with the Shelly's Bluetooth MAC from step 1.
If you already load this component through `external_components`, keep that
source and make sure it includes the pairing update.

```yaml
external_components:
  - source:
      type: local
      path: components
    components: [shelly_ble_rpc]

# Required for the newer Shelly's Just Works bonding.
esp32_ble:
  io_capability: none
  auth_req_mode: bond

esp32_ble_tracker:
  scan_parameters:
    active: false

ble_client:
  - id: shelly_client
    mac_address: AA:BB:CC:DD:EE:FF  # Replace with your Shelly's Bluetooth MAC.

shelly_ble_rpc:
  id: shelly_inputs
  ble_client_id: shelly_client
  pairing: true
  pairing_timeout: 30s
  update_interval: 5s
  response_timeout: 5s

  # Configure only inputs that actually exist on your Shelly.
  input_0:
    name: "Shelly Input 1"
    # on_delay: 10s  # Optional: delay ON by 10 seconds.

  connected:
    name: "Shelly Polling Healthy"
```

For devices with more inputs, keep or add `input_1`, `input_2`, and `input_3`
under `shelly_ble_rpc`, each with its own name. Leave `pairing` disabled on
older Plus i4 firmware; this example is specifically for bonding-capable devices.

In ESPHome Device Builder, save, validate, and install the updated firmware on
the ESP32 using your usual installation method. Wait until installation has
finished and the ESP32 is online again. Open its **Logs** view and keep it open.

Do this before the next step so the pairing window does not expire while the
firmware is being built or uploaded. Until pairing succeeds, pairing errors in
the logs and unknown input states are expected.

#### 3. Open the pairing window in your browser

Open a new browser tab. Append this path to the Shelly web address from step 1:

```text
/rpc/BLE.StartPairing?timeout=180
```

For example, **only if your Shelly is at `192.168.10.80`**, paste this complete
address into the browser's address bar and press Enter:

```text
http://192.168.10.80/rpc/BLE.StartPairing?timeout=180
```

Replace the example IP with your Shelly's IP. If its web interface uses
`https://`, use that same prefix here. Sign in with the Shelly's own web/admin
credentials if prompted.

Opening this address **sends the `BLE.StartPairing` command**. There is no
separate command to type into an ESPHome terminal. The browser should show:

```json
{"timeout":180}
```

This means the Shelly is accepting new pairings for three minutes. It does
**not** mean the ESP32 has already paired. Leave both devices powered and
within Bluetooth range, then return to the ESPHome log tab.

#### 4. Wait for the ESP32 to pair and read an input

The component requests pairing automatically when it connects. Look for these
messages, in this order (timestamps and log prefixes are omitted):

```text
Shelly BLE authentication successful (bonding enabled)
Shelly RPC service ready (authenticated BLE link)
First input poll successful
```

The first line confirms BLE authentication. The last line confirms that the
input readings work too. If configured, the polling-health entity turns ON
after a complete successful poll.

If the three-minute window expires, open the same start-pairing URL again.
If the ESP32 is not making another connection attempt, restart the ESP32 using
its normal restart control, or power-cycle the ESP32 while the window is open.
You do not need to reinstall firmware to retry pairing.

#### 5. Check the saved pairing and close the window

Using the same Shelly address, open:

```text
http://192.168.10.80/rpc/BLE.ListPairedDevices
```

Again, replace the example IP. The response should contain the ESP32's
Bluetooth address in an `addr` field. An empty list, `[]`, means no device is
saved. Other entries may belong to previously paired clients.

Once pairing has succeeded, open:

```text
http://192.168.10.80/rpc/BLE.StopPairing
```

A response of `null` is normal. This closes the window for new pairings;
it does not remove the saved pairing or stop input polling. The window also
closes automatically when its three-minute timer expires.

**Setup is finished.** Keep `pairing: true` in the ESPHome configuration.
The stored bond is intended to handle subsequent connections and reboots
without opening the pairing window again.

#### If it does not work

| What you see | What to do |
| --- | --- |
| The browser cannot reach the address | First make sure you can open the Shelly's ordinary web page. Check that you used the Shelly IP, not the ESP32 IP. |
| Browser says unauthorized / HTTP 401 | Use the Shelly's web/admin credentials. If the browser does not offer a login prompt, see the terminal alternative below. |
| `BLE.StartPairing` is unknown / method not found | Verify the device firmware supports this method. Leave older i4 devices on the existing legacy configuration. |
| Pairing fails or times out in ESPHome | Open the three-minute window again; check the Bluetooth MAC, range, and that the updated ESP32 firmware is installed. |
| Pairing succeeds, but `First input poll successful` never appears | Check that RPC is enabled and that the configured input IDs exist and are set to switch mode. Inspect the following error in the logs. |
| ESPHome reports RPC error 401 after BLE authentication | This is a separate RPC password challenge, not the browser login. Our component does not implement that challenge. |
| It used to work before a factory reset or flash erase | A stale bond may remain on one side. See the technical notes below; repeatedly opening the pairing window may not fix mismatched keys. |

##### Optional terminal alternative for password-protected devices

The browser method is sufficient when it works. If it returns HTTP 401 without
offering a login, open PowerShell on Windows and run this, replacing the example
IP with the Shelly's IP:

```powershell
curl.exe --digest --user admin "http://192.168.10.80/rpc/BLE.StartPairing?timeout=180"
```

Curl asks for the Shelly's admin password. Enter it and press Enter; it may not
display characters as you type. The password is the Shelly web/admin password,
not your Wi-Fi password or ESPHome API key. On Linux/macOS, use `curl` instead
of `curl.exe`. Use the same HTTP/HTTPS address as the Shelly web interface.

Successful output contains `"timeout":180`; continue at step 4. You can use
the same command with the paths `BLE.ListPairedDevices` or `BLE.StopPairing`
for the checks in step 5.

#### Technical notes

`esp32_ble` security settings apply to the entire ESP32 BLE stack. `pairing`
is per Shelly client, so legacy and paired instances can coexist. Explicit
`auth_req_mode: bond` or `sc_bond`, with `io_capability: none`, is required
when any instance enables pairing. The component does not silently modify
other clients' security settings. Just Works does not provide MITM protection.

ESPHome/ESP-IDF manages the keys and their persistent storage. Subsequent
connections request encryption using the stored bond and wait for successful
authentication before polling. This does not introduce periodic component
flash writes; Bluetooth's key storage is separate from meter checkpoints.

If a factory reset or flash erase removes a bond on one side, remove the stale
bond on the other side before pairing again. Shelly provides
`BLE.DeletePairedDevice`; ESPHome provides `ble_client.remove_bond` for its
client. The component deliberately never deletes bonds automatically.

#### Behavior and limitations

- `pairing: false` (default): no component-initiated pairing or authentication gate.
- `pairing: true`: require successful authentication with bonding enabled before
  any input RPC. Authentication failure, pairing timeout, or an unbonded peer
  causes disconnection; there is no fallback to legacy mode.
- `pairing_timeout` defaults to `30s`, accepts `5s` through `180s`, and is
  separate from the per-RPC `response_timeout`.
- RPC password/digest challenges (error 401) remain unsupported. BLE bonding
  is transport security, not an implementation of RPC password authentication.
- Only the Plus i4 on firmware 1.7.5 has been field tested. Bonded mode has
  passed configuration validation but still requires firmware compilation and
  physical pairing, reconnect, and reboot testing on a firmware 2.0 device.
- Native regression cases were added for the security gate, unrelated/late
  events, pairing failure/timeout, encrypted I/O, and reconnects. They were not
  compiled/run in this change, honoring the YAML-validation-only workflow.


A standalone example is available at [examples/shelly-bonded-ble.yaml](examples/shelly-bonded-ble.yaml).

### Polling and delays

The component reads configured inputs sequentially and processes their states
after the whole poll succeeds. This is not an atomic hardware snapshot. There
is an immediate poll after service discovery (and authentication in bonded mode),
followed by scheduled polling.

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
- **BLE pairing failed/timed out:** check the pairing window and saved bonds on both devices.
- **RPC error 401:** RPC password authentication is required; BLE bonding does not implement it.
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
python -m esphome config examples/shelly-bonded-ble.yaml
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
