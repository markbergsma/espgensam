# GLM bus decoding guide v3.0 — reconciliation

*Written 2026-11-18.*

A third-party reference, *Genelec GLM bus decoding guide v3.0*, became available after the
comparison in [`glm-protocol-comparison.md`](glm-protocol-comparison.md) was written. It covers
the speaker firmware's command dispatcher, the telemetry tags and the ISS settings in more depth
than either espgensam's captures or HLM's specification. The guide's author is
[OH2HUU](https://github.com/OH2HUU). The PDF is not redistributed with this repository;
contributors who have a copy keep it in the git-ignored `references/` folder. Below, "guide §N"
refers to its section numbers.

This document records what the guide changed in espgensam, what it confirmed, and what it
leaves open. Everything here is paraphrased. Exact frames are quoted only where they act as
test vectors.

---

## 1. Opcode names

Several opcodes were named after a guess at their purpose. The guide gives the firmware's own
names (guide §6.4), and `const.h` now uses them:

| Opcode | Old name | New name | What changed |
|---|---|---|---|
| `0x15` | `CMD_FLASH_COMMIT` | `CMD_SERVICE` | `0x15` is a service prefix. `15 33 n` stores the working settings into profile *n*, and `15 34 n` restores from it (guide §11.5). The builder is now `make_store_user_params()`. It is still never sent. |
| `0x17` | `CMD_PREPARE_CONFIG` | `CMD_NET_SETTINGS` | `17 01` tells the speaker to use its stored settings and ignore the DIP switches; `17 00` resumes the switches (guide §6.6). The builder is now `make_use_stored_settings()`. The frame sent is unchanged. |
| `0x22` | `CMD_HARDWARE_QUERY` | `CMD_SOFTWARE_QUERY_SHORT` | The reply is the short system-info string (model, class, firmware, build), not a hardware ID. |
| `0x2D` | `CMD_SESSION_PREAMBLE` | `CMD_SWITCH_BYPASS` | It is accepted without effect and without a reply on the firmware the guide examined. |
| `0x3C` | `CMD_UNKNOWN_3C` | `CMD_BASS_ANALOG_FILTER` | Analogue bass high-pass; one stored byte. |
| `0x3D` | `CMD_INPUT_SYNC` | `CMD_BASS_BYPASS` | It turns bass management on and off. It was never input synchronisation. |
| `0x42` | `CMD_UNKNOWN_42` | `CMD_DIGISUM_GAIN` | Digital-sum gain; one stored byte. |
| `0x10 01 09` | `DSP_LEVEL_UNKNOWN_09` | `DSP_LEVEL_TENTHS_DB` | The level sent again at 0.1 dB resolution. It is still not decoded, for the reason given in the comparison doc §7 item 7. |

New constants were also added for `0x43` (group delay), `0x4B` (bass-extension crossover) and
`0x53` (filter bypass).

## 2. Telemetry

### 2.1 Fixed: `0x49` misaligned the record scan

`0x49` is a one-byte round counter (guide §8). `parse_telemetry()` did not know this tag, so
it treated the operand as the next tag. When the counter happened to equal a meter tag, the
scan read a bogus record and skipped a real one. Firmware older than 1.14 does not send `0x49`,
which is why the original captures never showed the problem.

Two regression tests in [`tests/test_parse_telemetry.cpp`](../tests/test_parse_telemetry.cpp)
cover it. Both failed against the unfixed parser.

### 2.2 New sensors

| Tag | Meaning | Exposed as |
|---|---|---|
| `0x81` / `0x82` / `0x83` | Tweeter / midrange / woofer voice-coil temperature, `00 [°C]`. The values are modelled by the DSP, not measured. | `Tweeter/Midrange/Woofer Coil Temperature` |
| `0x88` | Main DC rail (about 51 V), as a percentage of its span | `Main Supply Voltage` |
| `0x89` | Low-voltage rail (about 22 V), as a percentage of its span | `Low Supply Voltage` |
| `0x84` | Processor load, the whole 16-bit word in per mille (`02 B3` = 69.1 %) | `Processor Load` |

These are diagnostic sensors, disabled by default. A sensor stays *unknown* if the speaker never
reports its tag. Two-way models have no midrange tag, only three-way models report the
supply rails, and `0x84` exists from firmware 1.12 onwards (guide §6.7, §8).

`0x84` also explains the device-class reading in comparison doc §4.2. Each class's load falls in
its own range, so the high byte alone sorts speakers: `01` subwoofer, `02` two- and three-way,
`03` in transition or standby. Without signal the load barely moves (the guide reports ±0.1 % in
a minority of replies; our two-way captures span 74.5-76.1 %). It shifts briefly after a store,
an apply or a wake-up. The sensor is diagnostic and does not measure audio activity.

The rail conversion is `base + span × percent / 100`. The base and span constants are in
`const.h` as `PSU_*`. Their value encoding is inferred from the guide's typical readings (70 %
→ 51.1 V, 62 % → 22.0 V) and has not been checked against a three-way speaker on this bus. The
guide notes that the 9301B and 9401A use a different formula, and espgensam does not handle it.

### 2.3 DIP-switch status

The guide's `55 84` reply contains three big-endian 32-bit words: the current DIP word, the
previous one, and a model-dependent level word. espgensam queries it once for each monitor during
discovery, and decodes the same reply when it observes GLM asking. The per-monitor diagnostic
text sensor shows the current word decoded for the monitor's model, for example
`Level -10 dB, ISS`. Raw words remain available in the logs. A monitor that does not return all
three words remains unknown rather than publishing an all-zero switch position.

The bit layout depends on the model family:

| Models | Bit table | Confidence |
|---|---|---|
| 8320A, 8330A | Guide §6.5 (bit 0 Stored, 1-2 Level, 3-4 Digital B/A, 5 ISS, 6 LED disable, 7-8 Treble tilt, 9-10 Bass tilt, 11 Desktop, 12-13 Bass roll-off) | Guide: software DIP word (no physical switches) |
| 8340A, 8350A, 8341A | Same | Guide: physical switches seen in the replies |
| 8331A, 8351A/B, 8361A, 1032C, S360A | Same | Inferred: their operating manuals show the identical 14-switch panel |
| 7350A | Guide §6.7.1 (bit 2 Stored, 3 ISS, 4 Test tone, 5-6 Level, 7 LFE +10 dB, 8 LFE channel A, 9 Digital LFE, 10-11 Phase, 12-13 Bass roll-off, 14 Bypass) | Guide: software DIP word (no physical switches) |
| 7360A, 7370A, 7380A, 7382A | Same as 7350A | Inferred: the panels carry the same functions; the guide has not confirmed their bit positions |
| Others (e.g. 1237A, 1238A, W371A) | None | Raw word only; their manuals show no DIP switches |

Paired switches whose effects add are shown as their sum (Level -10 and -20 dB as -30 dB, Treble
tilt +2 and -4 dB as -2 dB). Phase labels follow the subwoofer panel (-90° / -180°). Bits 31 and
30 are the status flags of guide §6.7.2 ("switches not valid or stored"); reading the pair in
order, they are shown as `Switches not valid (bit 31)` and `Stored override (bit 30)` on both
families. Any other bit the table does not define is shown as `Unknown bits`.

On the three-way monitors with a rotary level control (8331A, 8341A, 8351A/B, 8361A) the third
word is the knob position, `10 x (dB + 6)`, and is shown as `Level knob`. On two-way models it is
an internal level variable and is not shown.

### 2.4 Confirmed, not exposed

- **`0x8A`.** The high bits of the `0x49` counter, sent about every 30th reply. It is already
  consumed correctly by the generic two-byte `0x8x` rule.
- **The `0x06` marker.** The guide calls this the "busy" byte, seen around restarts. This
  matches §4.3 of the comparison doc: it is not a power state.

## 3. ISS sleep delay and sensitivity

The guide's ISS encodings (guide §11.2) match this project's own store capture
(comparison doc §9) exactly:

- `3A 01 [BE32]` is the delay in minutes. The firmware caps it at 14400. `FF FF FF FF` means
  Never.
- `3A 02 [BE32]` is the threshold in dBu, as a two's-complement integer. GLM's presets are Low
  −61, Medium −68 and High −75; the usable range is −80…−35.

The guide also gives the missing disabled ("Never") encoding and the remaining menu values.

**Implemented as runtime-only.** The hub exposes two selects, **ISS Sleep Delay** and **ISS
Sensitivity**. Each offers GLM's menu plus *Speaker default*. The values are broadcast, as GLM
sends them, but never followed by the `15 33 00` store that GLM sends. A store would write the
speaker's entire working state into its start-up profile, including whatever calibration a
group push left there.

The consequence follows from how the speakers behave. A speaker loses its runtime settings
when it wakes from ISS sleep or standby (guide §8.1 describes this as a restart; the README
already notes the lost settings). So the hub re-sends an override after every
discovery and group push, which it performs at every wake. An override therefore governs the
next sleep after each wake. The override is remembered on the controller across reboots.
Choosing *Speaker default* stops sending, and the speaker returns to its stored value at its
next restart.

Two more points affect sleep timing:

- **Polling holds the speakers awake.** Every `08` status query resets the speaker's ISS idle
  timer (guide §11.2), which matches the comparison doc §9 isolation trial. The control window
  must close before ISS can count down, and it already does.
- **Not yet verified on hardware.** It is not confirmed that a speaker applies `3A 01`/`3A 02`
  without the store. If it does not, these selects have no effect, and the only alternative
  would be to send the store.

## 4. Phase-correction bypass (`0x53`)

`53 00 [mask]` bypasses filters (guide §10.3):

- mask `01` bypasses the group-delay (phase-correction) FIR;
- mask `02` bypasses every other audio filter;
- mask `00` clears the bypass.

The masks do not combine.

**Implemented for mask `01` only**, as a hub-wide **Bypass Phase Correction** switch. It is
disabled by default and not persisted, like Bypass Calibration. It is sent unicast to each
monitor, re-sent after discovery and group pushes while on, and cleared with mask `00`. Mask
`02` is not offered: it would silently undo the room calibration and the factory driver EQ
together, and Bypass Calibration already covers the comparison users want.

A unicast setting is acknowledged with an empty `09` reply. Broadcasts are not acknowledged
(guide §6.4). That is why the bypass goes out once per monitor rather than being doubled back
to back, as the LED frames are.

## 5. Test vectors

[`tests/test_dsp_frame.cpp`](../tests/test_dsp_frame.cpp) checks the builders against the
guide's complete example frames, including the CRC:

- `3A 01` delays of 10 and 60 minutes;
- `3A 02` thresholds of −75 and −80 dBu;
- `53 00 01` and `53 00 00`.

All of them pass, which independently confirms the CRC and the escaping for these opcodes.

## 6. Still open

- Whether ISS writes take effect without `15 33 00` (§3).
- Whether every SAM model honours `53 00 01`. The guide's frames come from one speaker.
- The supply-rail value encoding on a real three-way (§2.2).
- `0x3C`, `0x3E` and `0x42` are documented as single stored bytes. espgensam's observations of
  them, including `0x3E` as LFE gain (comparison doc §7.1), are unchanged.
- `0x43` group delay and `0x4B` bass-extension crossover are named but not used.
