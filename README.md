# espgensam

**espgensam** is an **unofficial** [ESPHome](https://esphome.io/) component and firmware that turns an ESP32 with an RS-485 transceiver into a standalone controller for [**Genelec SAM (Smart Active Monitor) speakers**](https://www.genelec.com/sam-studio-monitors-subwoofers), communicating natively over the Genelec "GLM" bus and exposing monitor control directly to **Home Assistant**.

---

## Features

- **Native Home Assistant Integration**: Discovered automatically through the ESPHome Native API
- **Standalone Autonomy**: Controls monitors locally with zero dependency on the GLM network adapter, the GLM software or the Home Assistant server status.
- **Speaker Controls**: Direct volume, mute, power/standby, input select, and telemetry reporting.
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
