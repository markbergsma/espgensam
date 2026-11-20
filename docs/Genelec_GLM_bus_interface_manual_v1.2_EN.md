# GLM bus interface manual

Everything needed to exchange messages: frame, CRC, addresses, signals, bit layouts and conversion formulas — version 1.2

- **Author:** Pasi Vähämartti
- **Date:** 25 October 2026
**Markdown conversion:** Prepared with the author's permission. The original PDF is [Genelec_GLM_bus_interface_manual_v1.2_EN.pdf](./Genelec_GLM_bus_interface_manual_v1.2_EN.pdf).

## Contents

- [1. Introduction and scope](#1-introduction-and-scope)
- [2. Frame structure](#2-frame-structure)
  - [2.1 Reading a capture line](#21-reading-a-capture-line)
- [3. CRC-16/GSM](#3-crc-16gsm)
  - [3.1 Test vectors](#31-test-vectors)
- [4. Addresses and sessions](#4-addresses-and-sessions)
- [5. Inbound signals (hub → speaker)](#5-inbound-signals-hub--speaker)
- [6. Outbound signals (speaker → hub)](#6-outbound-signals-speaker--hub)
  - [6.1 Acknowledgements and identification](#61-acknowledgements-and-identification)
  - [6.2 Telemetry (reply to poll 08)](#62-telemetry-reply-to-poll-08)
  - [6.3 Read replies](#63-read-replies)
- [7. Converting raw values to real units](#7-converting-raw-values-to-real-units)
- [8. Bit layouts and field structures](#8-bit-layouts-and-field-structures)
  - [8.1 DIP word – two-/three-way models](#81-dip-word--two-three-way-models-8320a-8330a-8340a-8350a-8341a)
  - [8.2 DIP word – subwoofer](#82-dip-word--subwoofer-7350a-7360a7370a7380a7382a-similar)
  - [8.3 DIP-switch replies (command 55)](#83-dip-switch-replies-command-55)
  - [8.4 Mute and LED (2B)](#84-mute-and-led-2b)
  - [8.5 ISS sub-command byte (3A) and threshold presets](#85-iss-sub-command-byte-3a-and-threshold-presets)
  - [8.6 Level ids](#86-level-ids-10-01-nn-write--10-81-nn-read)
  - [8.7 Test signal (05)](#87-test-signal-05)
  - [8.8 Audio source (40) and bass crossover (3B)](#88-audio-source-40-and-bass-crossover-3b)
- [9. Complete command index](#9-complete-command-index)
- [Conversion and source notes](#conversion-and-source-notes)

## 1. Introduction and scope

This manual contains everything required to communicate with the RS-485 bus interface and to decode every value it carries: the frame structure, the CRC, addressing and sessions, the purpose of every inbound and outbound signal, the bit layouts of the fields (DIP words, mute/LED, replies), and the formulas that turn each raw value into its real unit. Filter mathematics, model-internal behaviour and capture statistics are out of scope; they are in the full decoding guide (version 2.11).

- **Parties:** the hub (adapter) is the bus master. Speakers and interface units are slaves. The host software on a computer drives the hub over USB.
- **Two directions:** hub → speaker (commands) and speaker → hub (acknowledgements, telemetry and read replies). A speaker never initiates traffic.

## 2. Frame structure

Every bus message is one frame:

```text
[address] [payload ...] [CRC high] [CRC low] [7E]
```

- **Address** — first byte, sent with the 9th bit set. It identifies the recipient (hub → speaker) or marks a speaker reply (chapter 4).
- **Payload** — the first byte is the command (hub → speaker) or the reply code `0x09` (speaker → hub). The rest are parameters.
- **CRC** — 16 bits, big-endian (high byte first).
- `0x7E` ends the frame.
- **Escaping:** a `0x7E` inside the payload or CRC is sent as `7D 5E`, and `0x7D` as `7D 5D`. Escaping is applied after the CRC is computed and removed on reception before the CRC is checked.

The manual's **GNET / RS-485 frame structure** illustration gives this example, with bytes shown in hex:

```text
9-bit address      CID + data       CRC-16/GSM       delimiter
      101             1F 00 67          9F 3C             7E
```

The diagram labels the address as a 9-bit address with the wake bit set, the CID as `CID_VOLUME_GLM = 0x1F`, the CRC parameters as polynomial `0x1021`, init `0x0000`, XOR `0xFFFF`, and `0x7E` as the end-of-frame delimiter.

### 2.1 Reading a capture line

On a capture line each field is a three-digit hex number (9 bits), e.g. `102 008 018 095 07E`.

- Fields that start with `1` (`102`, `1F0`, `1FF`, `101`) are address bytes (9th bit set). Fields that start with `0` are payload, CRC or the end flag.
- Here `102` = address (speaker with session ID 2), `008` = payload (telemetry poll), `018 095` = CRC, `07E` = end flag.
- A capture shows bytes already un-escaped, so `07E` can also appear mid-frame as data; only the last one is the end flag. The CRC is computed over the un-escaped bytes.

### 9N2 UART character and RS-485 differential pair

The timing illustration specifies a 9N2 UART character with an idle-high line, least-significant bit first, one start bit, nine data bits and two stop bits:

- One bit is approximately `3.47 µs`; one 12-bit character is `41.7 µs`.
- The illustrated data character has `D0` through `D4` set and `D5` through `D8` clear. Data bits are sent LSB first.
- The differential-pair legend is: mark when `A > B`, space when `A < B`, and idle is biased to mark. The example address character sets `D8 = 1` (for example, `0x101` at frame start).

The illustrated character's bit values are:

```text
idle  START  D0 D1 D2 D3 D4 D5 D6 D7 D8  STOP STOP  idle
  1      0    1  1  1  1  1  0  0  0  0    1    1     1
```

## 3. CRC-16/GSM

| Parameter | Value |
|---|---|
| Polynomial | `0x1021` |
| Initial value | `0x0000` |
| Input / output reflection | no / no |
| XOR-out | `0xFFFF` |
| Coverage | address low byte + all un-escaped payload bytes (not the CRC, not `7E`) |
| Byte order in the frame | high byte first |

Computation (pseudocode):

```text
crc = 0x0000
for each byte b in [address low byte] + payload:
    crc ^= b << 8
    repeat 8 times:
        crc = ((crc << 1) ^ 0x1021) if (crc & 0x8000) else (crc << 1)
        crc &= 0xFFFF
crc ^= 0xFFFF
```

The worked calculation uses address plus CID and data bytes `01 1F 00 67`:

| Step | Input byte | CRC register after byte (16-bit, MSB-first; no reflection) |
|---:|---:|---|
| 1 | `0x01` | `0x1021` = `0001000000100001` |
| 2 | `0x1F` | `0x0DEF` = `0000110111101111` |
| 3 | `0x00` | `0x247D` = `0010010001111101` |
| 4 | `0x67` | `0x05A7` = `0000010110100111` |

The final XOR with `0xFFFF` gives CRC `0xFA58`; the frame appends bytes `0xFA 0x58` before the `0x7E` delimiter. The figure also gives the check value `CRC-16/GSM("123456789") = 0xCE3C`.

### 3.1 Test vectors

If your implementation gives the same CRC for all of these, the parameters and byte order are correct (rows are from real captures).

| Address low byte + payload | CRC |
|---|---:|
| `01 09` | `0x5DE7` |
| `02 08` | `0x1895` |
| `FF 04` | `0xBC84` |
| `FF FE` | `0xF2D1` |
| `F0 1F 00 00 00` | `0x0FE0` |
| `FF 1F 00 31 07` | `0x2C5A` |
| `02 17 01` | `0x1B5A` |
| `02 2B 04` | `0x0B07` |
| `02 3B 00 01` | `0x37E2` |
| `02 3D 00 00` | `0x9563` |
| `FF 2D 00` | `0x4026` |

## 4. Addresses and sessions

| Address | Meaning |
|---|---|
| `0x1FF` | Broadcast to all speakers. Not acknowledged. |
| `0x1F0` | Speakers without a session (default RID F0). Not a group address: only speakers that do not yet have a session ID accept the frame. |
| `0x101` | Speaker reply to the hub (payload first byte is `09`). |
| `0x102` … `0x1EF` | A single speaker by its session ID (0x100 \| RID). |

Discovery and session setup:

- `FE` (broadcast) — discovery probe. A speaker without a session replies with its UID.
- `02 UID(3) RID` (address `0x1F0`) — assigns a session ID. The speaker accepts only its own UID and acknowledges `09 [RID]`. Session IDs come from the host software, not the hub.
- `04` (address `0x1FF`) — keep-alive. Comes from the host software; the hub only relays it. A speaker resets a 2 600 ms timer on every valid frame and keep-alive; when it expires the speaker resets its ID to F0.
- `03` (`SOFT_RESET`) — resets the session ID. Answered only to a unicast address.

Replies `09`, `10` and `11` are suppressed for addresses `0x1F0` and `0x1FF`. Exceptions: `FE` answers with its UID when the speaker's own ID is F0, and `02` acknowledges regardless of address.

## 5. Inbound signals (hub → speaker)

The first payload byte is the command, grouped below by purpose. A command sent to a unicast address is acknowledged (`09`) unless the handler returns its own reply; reply code 2 = command rejected (`10`), 3 = unsupported / invalid (`11`).

| Group | Command | Purpose |
|---|---|---|
| Discovery, session and link | `FE` | Discovery probe (broadcast); a session-less speaker replies with its UID. |
|  | `02` | Assign session ID (UID → RID), address `0x1F0`. |
|  | `04` | Keep-alive (broadcast). |
|  | `03` | Software reset (`SOFT_RESET`): session ID is cleared. |
| Volume and start-up level | `1F` | Volume: 24-bit linear gain (`0x7FFFFF ≈ 0 dB`). `0x1FF` not acknowledged, unicast acknowledged. Value 0 = mute; a zero at `0x1F0` does not mute. |
|  | `10 01 05` | Start-up volume (`StUpVol`): the start level set by the user in the host software, read by the speaker 3 s after start-up. Factory default `7F FF FF` (0 dB). |
| Input, source and bass management | `40` | Audio source selection: Main (idx 0) or LFE (idx 1); analog / digital (AES3). |
|  | `3B` | Bass-management crossover frequency (e.g. 85 Hz); value 1 = bypassed. |
|  | `3D` | Bass management on / off. `3D 00 00` is also sent to all after a settings block. |
|  | `3C`, `3E`, `42` | Subwoofer only: bass-management extra fields (A-filter, LFE gain, digital-sum gain). |
|  | `44` | Extended source selection (9301B and 9401A only). |
| Levels, delays and filters | `10 01` | Set max level or driver sensitivity (the id selects the target). |
|  | `10 02` | Node delay (32-bit sample count, 48 kHz). |
|  | `10 0E` | Write one room-EQ filter (idx `0x00`–`0x13`), coefficients float32. |
|  | `10 10`, `3F 10` | Tweeter-trim filter coefficients / parameters. |
|  | `53` | Filter bypass (mask). |
| Power, sleep, mute and LED | `3A` | ISS and power: sleep delay, threshold, state and LED. Writes are request codes (`00` short press, `01` wake, `02` long press = off), not direct states. |
|  | `2B` | Mute and LED channels: bit 0 = mute, bits 1–2 = LED channels, bit 3 = pattern word. |
|  | `05` | Test signal on/off and driver mute. |
|  | `2D` | `SWITCH_BYPASS`: no effect; sent before settings and at wake-up. |
| Storage, calibration and identifiers | `15 33` / `15 34` | Save user settings (location 0–3) / load them (0 = stored, 3 = DIP switches). |
|  | `34` / `35` | Save / restore user parameters. |
|  | `36` | Open factory writes (own UID + ISS active). Factory use. |
|  | `51` | Write name and notes (stored with user settings). |
| Queries (replies in chapter 6) | `08` | Telemetry poll for one speaker. Also resets the ISS sleep timer. |
|  | `22` / `39` | Short / long system info (model, version, build) in ASCII. |
|  | `2C 80` | Read UID (3 bytes). |
|  | `19` | Read serial number (ASCII). |
|  | `55 84` | DIP-switch state (reply 12 bytes). |
|  | `37`, `38`, `41`, `50` | Read driver data: gainride, driver protection, clip module, trace levels. |
| Firmware update | `15 2F` | Firmware update (start, data blocks). Sent to one speaker at a time. |

## 6. Outbound signals (speaker → hub)

The reply address is `0x101` and the first payload byte is `0x09`. The reply is an acknowledgement, a telemetry frame or a read reply.

### 6.1 Acknowledgements and identification

| Reply | Meaning |
|---|---|
| `09` (empty) | ACK: command received, no further data. |
| `09 [RID]` | Session ID accepted (reply to command `02`). |
| UID (3 bytes) | Reply to discovery (`FE`) or UID read; the speaker's unique identifier. |
| ASCII | System info (`22`/`39`), serial number (`19`) or name (`51`) per the query. |

### 6.2 Telemetry (reply to poll 08)

A telemetry frame starts with tag `41` (processor temperature) and contains the tags that changed since the previous poll (at most 40 bytes). A long reply may continue as a second frame without the `41`. Tags `0x81`–`0x84` and `0x88`–`0x8A` are 2 bytes, the rest 1 byte.

| Tag | Meaning |
|---|---|
| **State and counters** | |
| `0x41` | Processor temperature (°C). |
| `0x47` | Power state: `01` = on, `02` = standby, `03` = transition. |
| `0x49` | Counter: +1 per telemetry update, wraps `0xFF` → `0x00`; same ±1 across all speakers. |
| `0x84` | Processor load (16-bit per mille): `1000 − 1000 × idle / max idle seen`. |
| `0x8A` | High bytes of the same round counter as `49` (bits 8–23); reset only at start-up, so ≈ time since start-up. Newer firmware only. |
| **Level meters (dBFS = byte − 256)** | |
| `0x42` | Audio input level before volume (floor `0x89`). |
| `0x43` | Tweeter (TW) output meter after volume and mute (floor `0x80`). |
| `0x44` | Mid (MD) output meter (three-way; floor `0x80`). |
| `0x45` | Bass (BS) output meter (floor `0x80`). |
| `0x46` | LFE input peak (subwoofer only). |
| **Temperatures and voltages** | |
| `0x81` / `0x82` / `0x83` | Coil temperatures (°C): `81` = TW, `82` = MD, `83` = BS. Modelled, sent only in the on state. |
| `0x88` / `0x89` | Supply-voltage monitor percentages (`0x88` = psuMains, `0x89` = psu30v). M-family images only. |
| **Events (one-byte tag)** | |
| `0x02` / `0x03` | Protection started / ended. |
| `0x04` | Clipping. |
| `0x05` | Factory lock opened. |
| `0x06` / `0x07` | Digital input unavailable / available. |

### 6.3 Read replies

| Read command | Reply meaning |
|---|---|
| `10 81` / `10 82` | Sensitivity / delay by id. |
| `3A 81` / `3A 82` / `3A 83` | ISS: sleep delay (min) / threshold (dBu) / state (1 = Active, 2 = off, 3 = other). |
| `3B 80` … `3E 80` | Bass-management fields. |
| `53 80` | Filter-bypass mask (`00` = no bypass). |
| `2C` | UID. |

## 7. Converting raw values to real units

For every value read from the bus, the raw form and the formula that gives the real unit (BE = big-endian, LE = little-endian, 2's comp. = signed two's complement).

| Value (source) | Raw form | Conversion → unit |
|---|---|---|
| Volume (`1F`; read `10 81`) | 24-bit gain, BE | dB = `20 × log10(v / 0x7FFFFF)`. Inverse: `v = floor(10^(dB/20) × 0x7FFFFF)`. `0x7FFFFF ≈ 0 dB`, value 0 = mute. |
| Sensitivity / max level (`10 01`; read `10 81`) | 24-bit gain, BE | dB = `20 × log10(v / 0x7FFFFF)` (same as volume). |
| Level meters (telemetry `42`–`46`) | 1 byte | dBFS = byte − 256. ~1 dB per step; floor `0x80` = −128 dBFS. |
| Delay (`10 02`; read `10 82`) | 32-bit sample count, BE | ms = `n / 48` (48 kHz); µs = `n × 20.8`. |
| Processor temperature (`0x41`) | 1 byte | °C directly (already in Celsius; never below 20 °C). |
| Coil temperatures (`0x81`–`0x83`) | 2 bytes, `00 [value]` | °C directly (low byte = temperature; modelled, at rest = processor temperature). |
| Processor load (`0x84`) | 16-bit per mille, BE | % = value / 10 (e.g. `0x02B3` = 691 ‰ = 69.1 %). |
| Supply psuMains (`0x88`) | percentage | Value is already %. Voltage V ≈ `35 + value/100 × 23` (allowed 35–58 V). |
| Supply psu30v (`0x89`) | percentage | Value is already %. Voltage V ≈ `9 + value/100 × 21` (allowed 9–30 V). 9301B/9401A: V ≈ `8 + value/100 × 7`. |
| Round counter (`0x49`; high bytes `0x8A`) | 1 byte / 2 bytes | No unit (round count). `0x8A` ≈ time since start-up: seconds ≈ value × 256 (inferred). |
| ISS threshold (`3A 02`; read `3A 82`) | 32-bit 2's comp., BE | dBu directly (negative). Linear threshold = 10^(−\|dBu\|/10). E.g. `FF FF FF B5` = −75 dBu. |
| ISS sleep delay (`3A 01`; read `3A 81`) | 32-bit, BE | minutes directly (max 14400); 0 = never sleeps. `FF FF FF FF` stores as 65535 min = never. |
| ISS state (read `3A 83`) | 1 byte | No unit: 1 = Active, 2 = off, 3 = other / transition. |
| Analog input level (read `3A 84`) | 32-bit 2's comp., BE | dBu directly (`10 × log10` of internal power, 1 dB steps). E.g. `FF FF FF 96` = −106 dBu. |
| Crossover frequency (`3B`; read `3B 80`) | 16-bit, BE | Hz directly (5 Hz steps); 1 = bypassed. |
| Level potentiometer (`55 84` word 3, 8341A) | 32-bit, BE | dB = word / 10 − 6 (0 = −6 dB, 60 = 0 dB, 120 = +6 dB). |
| Driver protection (read `0x38`) | float32, LE (per group) | Limit = °C, gain = slope. Attenuation = `1 + slope × (limit − temperature)`, floor 0. |
| Filter coefficients (`10 0E`, `10 8F`…) | float32, LE | IEEE 754 float directly (e.g. `00 00 80 3F` = 1.0). |
| Phase (subwoofer DIP) | flight-time samples | 0 / 141 / 282 / 424 samples = 0° / 180° / 90° / 270° at 85 Hz (2.94 / 5.88 / 8.83 ms at 48 kHz). |
| System info (`22` / `39`) | ASCII | String directly, e.g. `c-2;model-8320A;ver-1.14.7.6281;hw-0.2.0;build-…`. |

## 8. Bit layouts and field structures

The exact bit patterns needed to decode the fields. Bit 0 is the least significant bit.

### 8.1 DIP word – two-/three-way models (8320A, 8330A, 8340A, 8350A, 8341A)

Read in word 1 of the `55 84` reply (or set by software with `55 01`). `00 00 00 01` means only the Stored bit is set (DIP switches ignored, stored settings used). Each switch is its own bit.

| Bit | Switch | Note |
|---:|---|---|
| 0 | Stored | 1 = ignore DIP switches, use stored settings. |
| 1 | Level −20 dB | Level bits 1–2: `00` = 0 dB, `01` = −20, `10` = −10, `11` = −30 dB. |
| 2 | Level −10 dB | (see bit 1). |
| 3 | Digital B | Digital input select (bits 3–4). |
| 4 | Digital A | (see bit 3). |
| 5 | ISS | Intelligent Signal Sensing enabled. |
| 6 | LED disable | 1 = LEDs off (same as `3A 06 = 01`). |
| 7 | Treble tilt −4 dB | |
| 8 | Treble tilt +2 dB | |
| 9 | Bass tilt −4 dB | |
| 10 | Bass tilt −2 dB | |
| 11 | Desktop −4 dB | Desktop / 160 Hz compensation. |
| 12 | Bass roll-off −4 dB | |
| 13 | Bass roll-off −2 dB | |

### 8.2 DIP word – subwoofer (7350A; 7360A/7370A/7380A/7382A similar)

| Bit | Switch | Note |
|---:|---|---|
| 2 | Stored | 1 = ignore DIP switches, use stored settings. |
| 3 | ISS | Intelligent Signal Sensing enabled. |
| 4 | Test tone 85 Hz | Built-in test tone. |
| 5 | System level −20 dB | Level bits 5–6: `00` = 0, `01` = −20, `10` = −10, `11` = −30 dB. |
| 6 | System level −10 dB | (see bit 5). |
| 7 | LFE +10 dB | LFE input +10 dB. |
| 8 | LFE channel 1 | LFE input channel select. |
| 9 | LFE digital | LFE input is digital. |
| 10 | Phase bit 0 (180°) | Phase bits 10–11: `00` = 0°, `01` = 180°, `10` = 90°, `11` = 270° (flight-time delay). |
| 11 | Phase bit 1 (90°) | (see bit 10). |
| 12 | Bass roll-off bit 0 (−4 dB) | Roll-off bits 12–13: `00` = 0, `01` = −4, `10` = −8, `11` = −12 dB (wide notch ≈ 76 Hz). |
| 13 | Bass roll-off bit 1 (−8 dB) | (see bit 12). |
| 14 | Bypass | Bass management bypass (LFE input only). |

### 8.3 DIP-switch replies (command 55)

| Command / reply | Structure |
|---|---|
| `55 84` → 12 bytes | Three 32-bit words: word 1 = current DIP word (8.1/8.2), word 2 = previous DIP word, word 3 = level (8341A: potentiometer, dB = word/10 − 6; others: \|signed level variable\|, same as `10 81 08`). |
| `55 01 w(4) f` | Write a 32-bit software DIP word plus a flag byte. |
| `55 81` → word | Read the software DIP word back. |
| `55 87` → id/value pairs | Active switch positions, e.g. `09 01` = ISS on, `0F B4` = phase 180°. If DIP-word bit 31 or 30 is set (not valid / stored), returns one pair `64 00` or `65 00`. |

### 8.4 Mute and LED (2B)

| Field | Meaning |
|---|---|
| bit 0 | Mute. |
| bits 1–2 | LED channels (both clear = LEDs off). |
| bit 3 | Selects the pattern word `0x33333333` instead of `0xFFFFFFFF`. |
| pattern word | 32 bits rotated one bit per slot (256 LED-timer ticks): `FFFFFFFF` = steady, `33333333` = two on / two off (blink). The speaker uses `55555555` itself in the 'protection active' state. |
| sub-command bytes | `03` = mute, `04` = unmute, `05` and `07` = mute at start-up, `08` = LEDs off. |

### 8.5 ISS sub-command byte (3A) and threshold presets

In `3A` the sub-command is bits 0–4 of the first payload byte and the flags are bits 5–7 (bit 7 = read). Sub-commands: 1 = sleep delay (minutes), 2 = threshold, 3 = state (request code), 4 = input level (read only), 6 = LED byte, 7 = stored byte, 9 / 0A = flags.

| Host preset | Threshold (`3A 02`) |
|---|---|
| Low | −61 dBu (`FF FF FF C3`). |
| Medium | −68 dBu (`FF FF FF BC`). |
| High | −75 dBu (`FF FF FF B5`). |
| Usable range | −80 … −35 dBu (threshold stored as \|x\|, 1 dB steps; linear = 10^(−\|x\|/10)). |

### 8.6 Level ids (10 01 nn write / 10 81 nn read)

| id | Meaning (id 04 does not exist) |
|---|---|
| `00` | Node level (max level). |
| `01`, `02` | Bass (BS) driver sensitivity. |
| `03` | Tweeter (TW) sensitivity. |
| `05` | Start-up volume (StUpVol). |
| `07` | DIP-switch gain (read only). |
| `08` | System level (read only; value shifted left 8 bits). |
| `09` | Level in 0.1 dB steps (read back × 10). |

### 8.7 Test signal (05)

| Field | Meaning |
|---|---|
| type (bits 0–4) | `3`, `4`, `5`, `7`, `8`, `9`, `10` start the generator; `0`–`2` and `6` stop it. `3` = white noise, `4` = pink noise, `5` = sine sweep, `7` = ID tone, `8` = sine generator. |
| flags (bits 5–7) | `0x40` = armed (next `05` within 3 s starts), `0x80` = read. |
| mask (24 bits) | Bits 0, 1, 23 select outputs (if none set, audio stops). Bit 20 mutes bass, 19 mid, 18 tweeter. Bit 21 adds a fixed 108-sample delay term. |
| frequency (p2) | Sine-generator frequency in Hz (85 for the DIP test tone). |
| level | Signed dB, clamped to −119 … +20 dBFS. |

### 8.8 Audio source (40) and bass crossover (3B)

| Command | Structure |
|---|---|
| `40 [idx] [src] [pair] [aes3]` | idx 0 = Main, 1 = LFE. src 1 = analog, 2 = digital (AES3), 3 = automatic. pair / aes3 = channel masks. |
| `3B [16-bit]` | Crossover in Hz (5 Hz steps). `3B 00 55` = 85 Hz; value 1 = bypassed (full range). |
| `3D 00 00` / `3D 00 01` | Bass management on / off. On a subwoofer, `01` forces the main-input gains to 0 so only the LFE input plays. |

## 9. Complete command index

All bus commands. Column W/R: W = write (hub → speaker), R = read (reply `0x101`). The read form is usually command + `0x80` (e.g. `2C 80`, `3A 82`). Availability: ‘all’ = in all 24 images, ‘M’ = M-family only (8331A–8361A, 7382A, W371A, 9301B, 9401A, BTM8381A, TOP8381A).

| Code | Name | W/R | Purpose and availability |
|---|---|---|---|
| `02` | `SET_RID` | W | Assign session ID (UID → RID), address `0x1F0`. All. |
| `03` | `SOFT_RESET` | W | Software reset; session ID cleared. All (9301A: stub). |
| `04` | `STAY_ONLINE` | W | Keep-alive (`0x1FF`). All. |
| `05` | `TEST_SIGNALS` | W | Test signal on/off, driver mute. All. |
| `06` | `WRITE_FLASH` | W | Flash write; only with the `15` prefix (factory). All. |
| `07` | `READ_FLASH` | R | Flash read. All. |
| `08` | `POLL` | W→R | Telemetry poll; reply telemetry or ACK. All. |
| `0F` | `MINMAX_SPL` | R | Returns the constant `85 34 85 34`. All. |
| `10` | `DSP_COEFF` | W+R | Levels/sensitivity (`10 01` / `10 81`), delay (`10 02` / `10 82`), room EQ (`10 0E`), tweeter trim (`10 10`). All. |
| `17` | `NET_SETTINGS` | W+R | Network flag; `17 04` reads 6 bytes; `17 01` pauses DIP switches. All. |
| `19` | `BAR_CODE` | R | Read serial number (ASCII). All. |
| `1A` | `BOOT` | W | Boot / restart. All. |
| `1B` | `VOLUME_IF` | W | Does not reply on the bus; use uncertain. All (9301A: stub). |
| `1F` | `VOLUME` | W | Volume (24-bit gain). All (9301A: stub). |
| `22` | `SOFTWARE_QUERY` | R | Short system info (ASCII). All. |
| `2B` | `GROUP` | W | Mute and LED channels. All. |
| `2C` | `UID_QUERY` | R | Read UID (`2C 80`). All. |
| `2F` | `FIRMWARE_UPDATE` | W | Update handler (via `15 2F`). All. |
| `33` | `STORE_USER_PARS` | W | Save user settings (`15 33 [location]`). All. |
| `34` | `RESTORE_USER_PARS` | W | Save user parameters. All. |
| `35` | `STORE_CALIBRATION` | W | Save factory calibration (mask; lock open). All. |
| `36` | `OPEN_FACTORY_LOCK` | W | Open factory lock (own UID + ISS active). All. |
| `37` | `GAINRIDE` | W+R | Gainride settings (read `37 80`). All. |
| `38` | `PROTECTION` | W+R | Driver-protection parameters (read `38 8x`). All. |
| `39` | `SOFTWARE_QUERY2` | R | Long system info (ASCII). All. |
| `3A` | `ISS` | W+R | Sleep delay, threshold, state, input level, LED and power. All. |
| `3B` | `BASS_MANAGE_XO` | W+R | Bass-management crossover (read `3B 80`). All. |
| `3C` | `BASS_AFILTER` | W+R | Bass-management A-filter (mainly subwoofer). All. |
| `3D` | `BASS_BYPASS` | W | Bass management on/off. All. |
| `3E` | `LFE_GAIN` | W+R | LFE gain (subwoofer). All. |
| `3F` | `DSP_PAR` | W+R | Tweeter-trim / filter parameters. All. |
| `40` | `SELECT_AUDIO_SOURCE` | W | Audio source (Main/LFE, analog/AES3). All (9301B/9401A: NoCanDo only). |
| `41` | `CLIP` | R | Clip-module data. All. |
| `42` | `DIGISUM_GAIN` | W | Digital-sum gain (subwoofer). All except 9301A. |
| `43` | `GROUPDELAY` | W+R | Group delay. M. |
| `44` | `SELECT_AUDIO_SOURCE32` | W+R | Extended source selection. 9301B and 9401A only; other M images return 3. |
| `4B` | `BASS_EXTENSION_XO` | W+R | Bass-extension crossover. M (W371A, BTM8381A: return 3). |
| `50` | `TRACE` | R | Trace levels. All. |
| `51` | `NAMES_USER` | W+R | Name / notes (`51 00`/`01`; read `51 80`). All. |
| `52` | `NAMES_FACTORY` | R | Factory name (`52 00`) / codec-chip list (`52 01`). All. |
| `53` | `BYPASS_FILTERS` | W+R | Filter-bypass mask (read `53 80`). All. |
| `54` | `ETH_MAC_ADDRESS` | W+R | MAC address. All. |
| `55` | `DIPSW` | R | DIP-switch state (`55 84`; `55 87` active positions). All. |
| `56` | `COOL_CTRL` | W+R | Cooling control. M (only 7382A and 8381A modules work; others return 3). |
| `57` | `DRIVER_DIAG` | R | Driver impedance diagnostics. 8381A modules, 9301B and 9401A only. |
| `58` | `SUBMODULE` | W+R | Sub-module link. 9401A only. |
| `7F` | `WAKEUP` | W | Request wake state. All. |
| `FE` | `RACE` | W→R | Discovery probe; session-less speaker replies with its UID. All. |

**Stub commands** (in every image; accepted, no effect, no reply): `09 0A 0B 0C 0D 0E 11 12 13 14 16 18 1C 1D 1E 20 21 23 25 26 27 28 29 2A 2D 2E 30 31 32`. These are in the table but the handler is empty. In the 9301A, `03`, `1B` and `1F` are also stubs. Command `31` (`LOAD_DSP_CODE`) is valid only after the `15` service prefix.

## Conversion and source notes

- The source's page 2 contains only the heading “Table of Contents”; the linked contents above were added for Markdown navigation.
- The source's page 3 frame illustration prints CRC bytes `9F 3C` for address + payload `01 1F 00 67`. The page 5 worked calculation prints `FA 58` for those same input bytes, and the stated CRC-16/GSM algorithm computes `FA58`. Both source values are retained above; the page 3 illustration's `9F 3C` does not match that calculation.
- The section text and numeric values are transcribed from the author's manual. Page illustrations have been represented as text and tables rather than embedded page images.
