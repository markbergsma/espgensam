# espgensam

**espgensam** is an **unofficial** [ESPHome](https://esphome.io/) component and firmware that turns an ESP32 with an RS-485 transceiver into a standalone controller for [**Genelec SAM (Smart Active Monitor) speakers**](https://www.genelec.com/sam-studio-monitors-subwoofers), communicating natively over the Genelec "GLM" bus and exposing monitor control directly to **Home Assistant**.

---

## Features

- **Native Home Assistant Integration**: Discovered automatically through the ESPHome Native API
- **Standalone Autonomy**: Controls monitors locally with zero dependency on the GLM network adapter, the GLM software or the Home Assistant server status.
- **Speaker Controls**: Direct volume, mute, power/standby, input select, and telemetry reporting.
- **Group Presets**: Switch between named calibrations (GLM's "groups") from Home Assistant, each with its own room EQ, level, delay, crossover and input routing per speaker. Convert an existing GLM setup file with the included tool.
- **Direct 9-Bit RS485 Transceiver**: Uses ESP32 RMT (10 MHz pulse digitization) for RX and RMT pulse generation for TX to cleanly handle the 9-bit/2-stop-bit GLM bus.

You may also want to take a look at [HLM, the Homebrew Loudspeaker Manager](https://github.com/robcazzaro/hlm), which is a similar project to control Genelec SAM monitors from a (STM32/ESP32) microcontroller. It already implements most of the protocol's functionality. We have started collaborating to better understand the underlying GLM protocol.

---

## Supported Hardware

### 1. M5Stack AtomS3 Lite + Atomic RS485 Base (`espgensam.yaml`)
- **MCU**: ESP32-S3 dual-core
- **RS485 TX**: `GPIO6`
- **RS485 RX**: `GPIO5`
- **Direction Control**: Automatic (Atomic RS-485 pulse-sensing circuit)
- **Status RGB LED**: `GPIO35` (WS2812)

While this hardware does work fine in practice, it has proven not to be ideal hardware for this use case due to the auto-direction circuit, and alternative hardware is currently under investigation.

---

## RJ45 "GLM" Cable Pinout

Connect the RS-485 transceiver terminal block to a standard CAT5/6 RJ45 patch cable wired to **T568B**:

| RJ45 Pin (T568B) | Wire Color | GLM Bus Signal | RS485 Terminal |
|---|---|---|---|
| **Pin 1** | White / Orange | **Data A (D+)** (non-inverting) | `A` |
| **Pin 2** | Orange | **Data B (D-)** (inverting) | `B` |
| **Pin 8** | Brown | **GND** (bus ground reference) | `GND` |
| Pins 3–7 | — | *Unconnected* | — |

---

## Configuration Reference

### 1. Declare Sub-Devices (`esphome: devices:`)
In `espgensam.yaml`, list the discrete monitor devices you want Home Assistant to create:

```yaml
esphome:
  name: "espgensam"
  friendly_name: "Genelec SAM Controller"
  devices:
    - id: dev_subwoofer
      name: "Subwoofer"
    - id: dev_left_monitor
      name: "Left Monitor"
    - id: dev_right_monitor
      name: "Right Monitor"
```

### 2. Hub & Monitor Configuration (`gensam:`)

```yaml
gensam:
  id: gensam_hub
  tx_pin: GPIO6
  rx_pin: GPIO5
  rx_buffer_size: 512

  # Coexistence with official GLM USB adapter
  yield_to_glm: true
  glm_inactivity_cooldown: 30s

  # Timing and filtering
  poll_interval: 1s                   # RS-485 physical keep-alive sampling
  telemetry_averaging_period: 60s     # In-memory averaging window for signal levels

  # Master volume mapping boundaries
  min_volume_db: -80.0                # Volume at slider = 0.0
  max_volume_db: 0.0                  # Volume at slider = 1.0
  startup_volume_db: -30.0            # Initial volume on boot

  # Diagnostic hub entities
  bus_status:
    name: "Bus Status"

  rediscover_button:
    name: "Rediscover Monitors"

  # Monitor bindings: associate physical speakers with Home Assistant devices
  monitors:
    - serial_number: "7350APM88123456"
      unique_id: 1842915
      name: "Subwoofer"
      device_id: dev_subwoofer
      # Optional. Fixes this speaker's routing at boot; omit it and the speaker keeps
      # whatever its own flash holds until a group is applied or you pick an option in
      # Home Assistant. One of: analog | aes3_a | aes3_b | aes3_sum
      input: aes3_sum

    - serial_number: "8330AP99234567"
      unique_id: 1654321
      name: "Left Monitor"
      device_id: dev_left_monitor

    - serial_number: "8330AP77345678"
      unique_id: 1987654
      name: "Right Monitor"
      device_id: dev_right_monitor

# Master group media player
media_player:
  - platform: gensam
    name: "Genelec SAM System"
```

Each monitor gets an **Input** select listing `Analog`, `AES3 Channel A (Left)`,
`AES3 Channel B (Right)` and `AES3 Channel A+B (Sum)`. Routing is per speaker because that is
how the hardware works: a GLM group can perfectly well run the subwoofer on AES3 while both
main monitors are analog, so there is no single system-wide input to select.

### 3. Group Presets (`gensam: groups:`)

A group preset is a named monitoring configuration, just like the Group buttons in GLM: which
speakers take part, how each is fed, and the room calibration for each of them at one listening
position. Switching between groups from Home Assistant re-sends the whole DSP
block to every speaker.

Because a group carries twenty EQ bands per speaker, groups live in their own file:

```yaml
gensam:
  groups: !include gensam_groups.yaml

  # Applied once the speakers have been found, so they are never left in a state
  # you did not choose. Defaults to the first group; use `none` to apply nothing.
  default_group: "Main Listening Position"

  group_select:
    name: "Group Preset"
```

```yaml
# gensam_groups.yaml
- name: "Main Listening Position"
  devices:
    - unique_id: 1842915          # matches a monitor's unique_id above
      source: aes3_sum            # analog | aes3_a | aes3_b | aes3_sum
      crossover: 90               # Hz
      level_db: -1.9258           # per-speaker trim from AutoCal
      delay_samples: 289          # alignment delay, 48 kHz samples
      filters:                    # up to 20; the rest are left flat
        - {type: notch, frequency: 56.1739, gain: -6.05847, q: 4.68839}
        - {type: low_shelf, frequency: 118.711, gain: -0.177536}
        - {type: high_shelf, frequency: 14999, gain: -0.0199986}
```

`type` is `notch` (a peaking filter, as GLM labels it), `low_shelf`, `high_shelf` or `bypass`.
Only `notch` takes a `q`. Set `enabled: false` on a device to mute it in that group rather
than configure it.

Filter order is the order the speaker's own filter slots run in, which differs by model: a
two-way monitor takes two low shelves, two high shelves and then up to sixteen notches, while
a subwoofer takes twenty notches and no shelves. The converter below gets this right; if you
write a group by hand, follow the same order.

Applying a group sets every speaker's Input select, so they always show what the speakers were
last told. Changing one by hand takes effect immediately but does not alter the group, so the
next group push - switching group, waking from standby, or a rediscovery - puts the group's own
routing back. While the two disagree, the hub's **Group Modified** diagnostic sensor is on.

### 4. Converting an existing GLM setup

`tools/sam2yaml.py` reads a GLM 5 `.sam` setup file and writes the groups file, so an
existing AutoCal calibration does not have to be retyped:

```bash
python3 tools/sam2yaml.py "My Setup.sam" \
    --monitors 1842915,1654321,1987654 \
    -o gensam_groups.yaml
```

`--monitors` lists the `unique_id`s from your `monitors:` block. Devices outside that list are
skipped with a warning — a GLM setup file can retain a speaker that is no longer connected, or
one that was never really there.

Anything the tool cannot carry across is reported on stderr rather than dropped quietly. Read
those warnings: they are the difference between the group sounding as GLM calibrated it and
sounding close.

The tool needs PyYAML. If your system Python lacks it, `pip install pyyaml`, or run it with
ESPHome's own interpreter.

---

## Getting Started

### 1. Configure Secrets
```bash
cd espgensam
cp secrets.yaml.example secrets.yaml
```
Edit `secrets.yaml` with your Wi-Fi credentials and ESPHome API key.

### 2. Build & Flash

For **M5Stack AtomS3 Lite**:
```bash
esphome run espgensam.yaml
```

---

## License

This project is licensed under the [GNU General Public License v3.0](LICENSE).
