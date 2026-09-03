# espgensam

**espgensam** is an [ESPHome](https://esphome.io/) component and firmware that turns an ESP32 with an RS-485 transceiver into a standalone **Genelec SAM (Smart Active Monitor)** controller, communicating natively over the Genelec "GLM" bus and exposing monitor control directly to **Home Assistant**.

---

## Features

- **Native Home Assistant Integration**: Discovered automatically through the ESPHome Native API (encrypted, zero polling latency, no custom integration required).
- **Direct 9-Bit RS485 Transceiver**: Uses ESP32 RMT (10 MHz pulse digitization) for RX and RMT pulse generation for TX to cleanly handle the 281,250 baud 9-bit/2-stop-bit GLM bus with zero inter-byte gaps.
- **Standalone Autonomy**: Controls monitors locally with zero dependency on the Home Assistant server status.
- **Low-Latency Controls**: Direct volume, mute, power/standby, input select, and telemetry reporting.

---

## Supported Hardware

### 1. M5Stack AtomS3 Lite + Atomic RS485 Base (`espgensam.yaml`)
- **MCU**: ESP32-S3 dual-core
- **RS485 TX**: `GPIO6`
- **RS485 RX**: `GPIO5`
- **Status RGB LED**: `GPIO35` (WS2812)

### 2. LilyGO T-CAN485 (`espgensam-tcan485.yaml`)
- **MCU**: ESP32 dual-core
- **RS485 TX**: `GPIO22`
- **RS485 RX**: `GPIO21`
- **RS485 AutoDirection / RX**: `GPIO17`
- **Transceiver Enable (SE)**: `GPIO19`
- **Power Enable (5V Booster)**: `GPIO16`
- **Status RGB LED**: `GPIO4` (WS2812)

---

## RJ45 "GLM" Cable Pinout

Connect the RS485 transceiver terminal block to a standard CAT5/6 RJ45 patch cable (T568B):

| RJ45 Pin (T568B) | Wire Color | GLM Bus Signal | RS485 Terminal |
|---|---|---|---|
| **Pin 1** | White / Orange | **Data A (D+)** (non-inverting) | `A` |
| **Pin 2** | Orange | **Data B (D-)** (inverting) | `B` |
| **Pin 8** | Brown | **GND** (bus ground reference) | `GND` |
| Pins 3–7 | — | *Unconnected* | — |

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

For **LilyGO T-CAN485**:
```bash
esphome run espgensam-tcan485.yaml
```
