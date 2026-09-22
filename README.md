# espgensam

**espgensam** is an **unofficial** [ESPHome](https://esphome.io/) component and firmware that turns an ESP32 with an RS-485 transceiver into a standalone controller for [**Genelec SAM (Smart Active Monitor) speakers**](https://www.genelec.com/sam-studio-monitors-subwoofers), communicating natively over the Genelec "GLM" bus and exposing monitor control directly to **Home Assistant**.

---

## Features

- **Native Home Assistant Integration**: Discovered automatically through the ESPHome Native API
- **Standalone Autonomy**: Controls monitors locally with zero dependency on the GLM network adapter, the GLM software or the Home Assistant server status.
- **Speaker Controls**: Direct volume, mute, power/standby, input select, and telemetry reporting.
- **Group Presets**: Switch between named calibrations (GLM's "groups") from Home Assistant, each with its own room EQ, level, delay, crossover and input routing per speaker. Read straight from your existing GLM 5 setup file.
- **Direct 9-Bit RS485 Transceiver**: Uses ESP32 RMT (10 MHz pulse digitization) for RX and RMT pulse generation for TX to cleanly handle the 9-bit/2-stop-bit GLM bus.

You may also want to take a look at [HLM, the Homebrew Loudspeaker Manager](https://github.com/robcazzaro/hlm), which is a similar project to control Genelec SAM monitors from a (STM32/ESP32) microcontroller. It already implements most of the protocol's functionality. We have started collaborating to better understand the underlying GLM protocol.

---

## Supported Hardware

There is one config, `espgensam.yaml`. Selecting a board is a one-line change to its `packages:` block:

```yaml
packages:
  # --- Board selection: exactly one of these ---
  board: !include packages/board_m5stack_atoms3_rs485_base.yaml
  # board: !include packages/board_waveshare_esp32s3_rs485_can.yaml

  gensam: !include packages/gensam.yaml
```

Everything else — monitors, entities, tunables — lives in `packages/gensam.yaml` and is shared. Adding a board means adding one `packages/board_*.yaml`.

### 1. M5Stack AtomS3 Lite + Atomic RS485 Base
- **MCU**: ESP32-S3 dual-core
- **RS485 TX**: `GPIO6`
- **RS485 RX**: `GPIO5`
- **Direction Control**: Automatic (Atomic RS-485 pulse-sensing circuit)
- **Status RGB LED**: `GPIO35` (WS2812)
- **Rediscover button**: `GPIO41`

While this hardware does work fine in practice, it has proven not to be ideal hardware for this use case due to the auto-direction circuit, and alternative hardware is currently under investigation.

### 2. Waveshare ESP32-S3-RS485-CAN
- **MCU**: ESP32-S3 dual-core
- **RS485 TX**: `GPIO17`
- **RS485 RX**: `GPIO18`
- **Direction Control**: Explicit, `GPIO21` (SP3485EN; `/RE` is tied to `DE`, so the receiver is disabled while transmitting)
- **Isolation**: Galvanically isolated RS-485, with an onboard 120 Ω termination and 4.7 kΩ fail-safe bias
- **Status RGB LED**: none — the onboard LEDs are power and bus-activity indicators. Bus state is reported through the **Bus Status** sensor in Home Assistant.
- **Rediscover button**: none — use the **Rediscover Monitors** button entity.

Under evaluation, to test whether an explicitly driven direction line and a better receive path reduce the reply loss seen on the auto-direction module. Two things to be aware of before relying on it:

- **Make sure the bus has exactly one terminator at the controller end.** This matters more than anything else in the wiring, and it is easy to get wrong in both directions. Measured on this board, 4,000+ frames per run:

  | Terminators | `C0` address aliases | Rejected start edges |
  |---|---|---|
  | 0 | **71% of frames** | 1.90% of characters |
  | 1 (120 Ω) | 0.87% | 0.027% |

  Unterminated, the line takes ~4.7 µs to coast back to its idle level after the driver releases — longer than a bit time — so monitors replying 5–10 µs later lose their start edge. Frames still arrive intact either way, but only because the address-alias workaround is repairing 71% of them.

  In practice: the board ships with its 120 Ω fitted, and a GLM adapter carries one too — a passive resistor, so it terminates whether or not the adapter is powered. **Adapter attached → pull the Waveshare jumper. Waveshare alone → leave it in.**

---

## RJ45 "GLM" Cable Pinout

Connect the RS-485 transceiver terminal block to a standard CAT5/6 RJ45 patch cable wired to **T568B**:

| RJ45 Pin (T568B) | Wire Color | GLM Bus Signal | RS485 Terminal |
|---|---|---|---|
| **Pin 1** | White / Orange | **Data A (D+)** (non-inverting) | `A` |
| **Pin 2** | Orange | **Data B (D-)** (inverting) | `B` |
| **Pin 8** | Brown | **GND** (bus ground reference) | `GND` |
| Pins 3–7 | — | *Unconnected* | — |

The `GND` row is board-dependent. **The Waveshare's RS-485 terminal block has only `A` and `B`** — there is no ground terminal, so pin 8 is simply left unconnected there, and the board works correctly that way. Its fail-safe bias network ties `A` to the isolated 3V3 rail and `B` to the isolated ground, so the isolated side already self-references to the bus through 4.7 kΩ and never actually floats.

A defined ground reference is still better practice on long runs or in electrically noisy installations, where common-mode could otherwise drift outside the transceiver's −7 V…+12 V input range. On this board the only access is the internal pin header. It would create no ground loop — the isolation barrier blocks any return path through mains earth — but it has not proved necessary in practice.

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

The pin keys belong in a board package; everything else is board-independent and lives in `packages/gensam.yaml`. They are shown together here for reference.

```yaml
gensam:
  id: gensam_hub

  # --- Transceiver pins (board package) ---
  tx_pin: GPIO6
  rx_pin: GPIO5
  # de_pin: GPIO21      # Direction line, driven per frame. Omit on auto-direction modules.
  # re_pin: GPIO17      # Receiver enable, asserted once at boot.
  # power_pin: GPIO16   # 5 V DC-DC booster enable, for modules with their own rail.
  # se_pin: GPIO19      # Transceiver enable / shutdown, asserted once at boot.
  # tx_echoes_rx: false # Whether any of our own transmission can reach RX. Defaults to
  #                     # "no de_pin configured", which is right for every supported board. An
  #                     # auto-direction module's one-shot is triggered by our own start bit, so
  #                     # it is always late and the opening bits escape; a deliberately driven
  #                     # direction line is asserted first and leaks nothing. Set it only for a
  #                     # board that drives DE but leaves its receiver permanently enabled —
  #                     # getting it wrong the other way discards the start of every reply.
  # All pin keys accept `inverted: true`. `mode:` is ignored: the driver configures pull-ups
  # itself, and RMT rebinds tx_pin/rx_pin regardless.

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

Each monitor also gets three calibration controls: **Bass Management Crossover Frequency**
(50-120 Hz), **Level** (-60 to 0 dB, attenuation only) and **Delay** (0-192 ms). They are
disabled by default, because a group preset normally owns them and overwrites them at its next
push — enable them in Home Assistant for a speaker you want to trim by hand.

### 3. Group Presets (`gensam: groups:`)

A group preset is a named monitoring configuration, just like the Group buttons in GLM: which
speakers take part, how each is fed, and the room calibration for each of them at one listening
position. Switching between groups from Home Assistant re-sends the whole DSP
block to every speaker.

Most setups should get their groups from their existing GLM calibration, with
[`sam_file:`](#4-importing-an-existing-glm-setup) below. Written out by hand instead, a group
carries twenty EQ bands per speaker, so they live in their own file:

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
      delay_samples: 289          # alignment delay, 48 kHz samples (max 9216 = 192 ms)
      lfe_channel: aes3_b         # subwoofers in surround setups only; default none
      lfe_level_db: -4            # whole dB, including the LFE +10 boost if set
      filters:                    # up to 20; the rest are left flat
        - {type: notch, frequency: 56.1739, gain: -6.05847, q: 4.68839}
        - {type: low_shelf, frequency: 118.711, gain: -0.177536}
        - {type: high_shelf, frequency: 14999, gain: -0.0199986}
```

`type` is `notch` (a peaking filter, as GLM labels it), `low_shelf`, `high_shelf` or `bypass`.
Only `notch` takes a `q`. Set `enabled: false` on a device to mute it in that group rather
than configure it.

`lfe_channel` is for a subwoofer in a surround setup, where the discrete ".1" channel reaches
it on its own input alongside the bass-managed program. Leave it out for stereo and 2.1, which
is what `none` means. When it is set, `source` must name a single channel rather than the A+B
sum, so that the LFE feed is not also folded into the program path; importing handles this for
you.

Filter order is the order the speaker's own filter slots run in, which differs by model: a
two-way monitor takes two low shelves, two high shelves and then up to sixteen notches, while
a subwoofer takes twenty notches and no shelves. Importing gets this right; if you write a
group by hand, follow the same order.

Applying a group sets every speaker's Input select, Crossover, Level and Delay - and the LFE
routing and level on a subwoofer that has them - so they always show what the speakers were
last told. Changing one by hand takes effect immediately but does
not alter the group, so the next group push - switching group, waking from standby, or a
rediscovery - puts the group's own values back. While the two disagree, the hub's **Group
Modified** diagnostic sensor is on.

### 4. Importing an existing GLM setup (`gensam: sam_file:`)

Point the hub at a GLM 5 `.sam` setup file and every group in it becomes a group preset, so an
existing AutoCal calibration does not have to be retyped:

```yaml
gensam:
  monitors:
    - unique_id: 1842915
      # ...

  sam_file: "My Setup.sam"

  default_group: "Main Listening Position"
  group_select:
    name: "Group Preset"
```

The file is read while ESPHome validates the configuration, which happens on every
`esphome config`, `compile` and `run`. Re-run calibration, save in GLM, rebuild: there is nothing
to regenerate and no converted file to keep in step. `esphome config` prints the groups in
full, which is how you see what was imported.

Presets appear in the order the setup file lists them, before any you also wrote in `groups:`.
That order is what the **Group Preset** select offers and what `default_group` falls back to,
so a hand-written extra — a mute-all, a late-night trim — lands after the calibrated positions.

Only devices named in your `monitors:` block are configured. The rest are skipped with a
warning: a GLM setup file can retain a speaker that is no longer connected, or one that was
never really there. With no `monitors:` at all, every device in the file is taken as yours.

Anything the import cannot carry across is reported rather than dropped quietly. Pay attention
to those warnings: they are the difference between the group sounding as GLM calibrated it,
and sounding off.

#### Where the file lives

The path is relative to the directory holding your ESPHome YAML, but `~` is expanded and an
absolute path is taken as given, so it can point straight at GLM's own setup directory:

```yaml
  sam_file: "glm/My Setup.sam"
  sam_file: "~/Documents/Genelec/GLM5/Setup Files/My Setup.sam"
```

Building from the ESPHome dashboard or the Home Assistant add-on needs the `.sam` copied into
the configuration directory instead. GLM setup names usually contain spaces, so quote them.

#### If you would rather have the YAML

`tools/sam2yaml.py` runs the same conversion and writes the groups file out, which is the way
to inspect it, diff two GLM exports, or correct a file the component will not accept:

```bash
python3 tools/sam2yaml.py "My Setup.sam" \
    --monitors 1842915,1654321,1987654 \
    -o gensam_groups.yaml
```

`--monitors` does by hand what `monitors:` does automatically. Include the result with
`groups: !include gensam_groups.yaml` and leave `sam_file:` out. The tool needs PyYAML; if your
system Python lacks it, `pip install pyyaml`, or run it with ESPHome's own interpreter.

---

## Getting Started

### 1. Configure Secrets
```bash
cd espgensam
cp secrets.yaml.example secrets.yaml
```
Edit `secrets.yaml` with your Wi-Fi credentials and ESPHome API key.

### 2. Build & Flash

Select your board in the `packages:` block of `espgensam.yaml` (see [Supported Hardware](#supported-hardware)), then:

```bash
esphome run espgensam.yaml
```

Monitor serial numbers and entities live in `packages/gensam.yaml`, so they carry over unchanged if you swap boards.

---

## License

This project is licensed under the [GNU General Public License v3.0](LICENSE).
