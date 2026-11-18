# espgensam vs. HLM — GLM RS-485 protocol comparison

*Written 2026-09-19, comparing espgensam at `968fdc4` against HLM at `d51ab87`.*

## Context

Two independent reverse-engineering efforts on the Genelec GLM RS-485 bus now exist side by side:

- **espgensam** (this repository) — ESPHome external component, C++17, targets ESP32, integrates with Home Assistant, designed to **tolerate** a real GLM adapter on the same bus: when one is detected it yields completely, stops transmitting and does nothing but snoop until the bus goes quiet again. Protocol knowledge lives in header banners plus three raw bus captures of OEM GLM v5 traffic in [`captures/`](../captures) (15.7k lines, a 7350A sub + two 8330A).
- **HLM** — "Homebrew Loudspeaker Manager", <https://github.com/robcazzaro/hlm>, bare-C firmware for STM32F103/F407 and ESP32-S3/C6, a standalone OLED+encoder hub that **replaces** the GLM adapter. Published 2026-09-18 by Rob Cazzaro. Reverse-engineered from two 8320A with LLM assistance. Ships a 313-line formal specification with per-command confidence levels: `docs/Genelec GLM Protocol.11.md`.

File references below are relative to each project's own root: bare paths such as `components/gensam/const.h` mean espgensam, and paths prefixed `hlm/` mean the HLM tree.

The two were developed with no contact. That makes the overlap a genuine cross-validation, and the disagreements unusually informative: each project saw things the other did not (espgensam: a subwoofer alongside two-way monitors, and OEM adapter traffic; HLM: a signal generator, PEQ editing and flash-commit flows driven from the PC app).

This document records what agrees, what conflicts, and which conflicts the espgensam captures settle. §7 lists open items and §8 the experiments that would settle them.

---

## 1. Independently confirmed (both arrived at the same answer)

This is the load-bearing part of the protocol, and it is now confirmed twice over from separate captures and separate hardware:

| Layer | Agreed definition |
|---|---|
| Wire | RS-485 half-duplex, 9 data bits, no parity, **2 stop bits**; 9th bit = 1 marks the address byte, 0 everything else |
| Framing | HDLC-like, **end flag only** (`0x7E`), no preamble, no length field |
| Escaping | `0x7E → 7D 5E`, `0x7D → 7D 5D`, applied to payload + CRC, **never to the address byte** |
| CRC | **CRC-16/GSM**: poly `0x1021`, init `0x0000`, RefIn/RefOut false, XorOut `0xFFFF`; scope = address byte (low 8 bits) + unescaped payload; **big-endian** on the wire; escaped like any other byte |
| Addressing | `0x01` host · `0x02…` dynamically leased device IDs · `0xF0` multicast · `0xFF` broadcast · all device replies come from `0x01` |
| Discovery | `FF FE` probe → device replies with 3-byte hardware ID → `F0 02 [id0 id1 id2][newaddr]` → device acks with its new address |
| Volume | 24-bit **linear gain**, big-endian, `round(10^(dB/20) × 2^23)`, broadcast on `0x1F`, fire-and-forget |
| Opcodes | `02` assign ID · `04` broadcast sync · `08` telemetry poll · `09` reply · `19` serial · `1F` volume · `2B` mute/front-LED presentation · `39` info · `3A` ISS settings/power · `3B` bass mgmt · `40` input select · `FE` discovery |
| Telemetry | Tag/value stream after `09 41`, tags `41 42 43 45 47 81 83 84`, `47 01` = active / `47 02` = standby |
| Power-off | `3A 03 02` ×2 then `3A 03 00` ×2 |

The espgensam captures also confirm two of HLM's numbers to the millisecond:

- HLM documents the OEM hub emitting `F0 1F 00 00 00` every ~610 ms as a null/defer heartbeat while the PC app owns the bus. The startup capture shows `addr=0xF0 cmd=0x1F payload=[00 00 00]` at 21:53:25.305 / .917 / 26.528 — **611 ms**, 755 occurrences.
- HLM documents `1F 00 00 08` followed by `1F 00 00 02` as the store/apply duck. The capture has both (`00 00 08` ×3, `00 00 02` ×7) on `0xFF`. espgensam independently picked `00 00 02` as its source-switch silence value — the same value the OEM uses to duck.

A minor modelling difference with one downstream consequence: espgensam treats the byte after the address as a **command byte** separate from the payload; HLM treats the whole thing as a flat payload starting with `0x09`. HLM's flat model produced a parsing artifact — a branch at `hlm/src/HLM/glm_library.c:1006-1012` that decodes `5D E7` as if it were payload when it is actually the CRC of a bare `09` ACK. espgensam sees the same frame cleanly as `cmd=0x09 payload=[]`.

---

## 2. The headline difference: baud rate

| | espgensam | HLM |
|---|---|---|
| Configured at comparison time | `281250` (`components/gensam/const.h:13`) | `296000` (`hlm/src/STM32F407/Core/Src/main.c:497`, `hlm/src/ESP32Common/rs485_9n2.c`) |
| Actually generated at comparison time | 281,250 exactly (RMT, Q16 fixed-point accumulator, zero drift) | **296,296** on both ports — ESP32 `ticks_per_bit = 80e6/296000 = 270` → 296,296; STM32F103 USART1 @72 MHz quantises USARTDIV to 15.1875 → 296,296 |

**These differ by +5.35%, which is outside what a conventional 12-bit UART frame tolerates.** The last sampled bit (first stop bit) sits 10.5 bit-times after the start edge, so the error budget is |err| < ~4.8%. Both could not be right about the true bus rate, and the prediction was that **neither is** — the real rate lying between them, leaving each about ±2.6% off and comfortably inside budget, with **288,000 = 6 × 48,000** the attractive candidate in that gap for a device whose DSP runs at 48 kHz (and exactly representable on a 72 MHz STM32, USARTDIV = 15.625, the class of part inside the real GLM adapter).

### Measured: ~288,000 baud

This has now been measured directly on a live bus (espgensam `baud_sweep`, passive, against a GLM adapter running standalone). Rather than testing whether a rate decodes, the measurement fits the bit period to the raw edge timings: the interval between successive falling edges spans a whole number of bit periods, so the period is the value leaving every observed interval nearest a whole multiple.

```
Bit period fit over 7380 pulses: 34.74 ticks (287773 bps), mean residual 0.263 ticks
  busiest durations (ticks x count): 139x2302 278x823 174x812 104x774 209x561 208x423
```

At 34.74 ticks/bit every populated cluster lands within 0.016 bit of a whole multiple — 139 → 4.001 bits, 278 → 8.002, 174 → 5.009, 104 → 2.994 — and the mean residual is 0.26 ticks, i.e. 0.76% of a bit, which is line jitter. Scoring the candidates against those same clusters:

| Candidate | ticks/bit | mean bit-fraction error |
|---|---|---|
| 281,250 (espgensam) | 35.556 | 0.128 |
| **288,000 (6 × 48 kHz)** | **34.722** | **0.008** |
| 296,296 (HLM) | 33.750 | 0.169 |
| measured | 34.750 | 0.005 |

The measured 287,773 bps is **0.079% from 288,000**, and 288,000 fits 16–21× better than either implementation's rate. At comparison time espgensam ran 2.27% low and HLM 2.88% high, both inside a normal UART's budget. One caveat — the measurement is relative to the ESP32's own 10 MHz RMT clock, so a small crystal offset could account for that last 0.08%; given 6 × 48 kHz is exact, 288,000 is almost certainly the intended figure.

**Current implementation:** espgensam now configures 288,000 baud (`components/gensam/const.h`). The 281,250 figures and the earlier decision discussion below describe the comparison-time firmware.

### Why decode-based testing cannot show this

A first attempt scored each candidate by decode failures on identical input. It returned nothing: **every rate from 281,250 to 296,296 decoded byte-identically with zero faults**, at 3936 characters and counting.

That null result is itself the explanation for how two projects 5.35% apart both work. espgensam's receiver re-synchronizes on every character's start edge, accepts the stop bit at either of two positions, and treats "past the end of the burst" as idle HIGH. Its binding constraint is the 9th-bit sample at 9.5 bit periods (9.5 × 5.35% = 0.508 bit), and even that only bites when adjacent bits differ. The receiver simply tolerates the whole disputed span — so decode success measures tolerance, not line rate. Only the edge timings measure the rate.

---

## 3. Physical-layer engineering (different solutions, same problem)

Both had to produce 9-bit UART on an ESP32, which has no native 9-bit mode.

| | espgensam | HLM |
|---|---|---|
| TX | RMT pulse train, whole frame as one burst, **0 ns inter-byte gap** (required so auto-direction transceivers don't drop DE mid-frame) | RMT, per-word symbol generation, 270 ticks/bit |
| RX | Custom RMT **pulse digitizer** in an ISR: edge hunt, start-bit verify, centre-sample, 9th bit, stop-bit check with a retry at stop-bit-2 centre | Hardware UART configured **8N2** plus the **framing-error trick**: the 9th bit occupies the first stop slot, so `frm_err == false` ⇒ 9th bit was 1 (`hlm/src/ESP32Common/rs485_9n2.c:88`). RX FIFO threshold forced to 1 byte so the error flag stays aligned with its byte |
| DE/RE | Optional; reference board uses **none** (auto-direction), TX GPIO pre-driven high so DE doesn't latch. Echo suppressed by skipping exactly the TX duration in RMT ticks | Explicit DE/RE GPIO. ESP32 asserts DE then waits a full **1 ms** before transmitting; STM32 asserts and transmits immediately (the two ports differ by ~1 ms of turnaround) |
| Portability | ESP32-only by construction | STM32F103, STM32F407, ESP32-S3, ESP32-C6 |

HLM's framing-error trick is the more economical idea and is why it ports to a plain STM32 USART. espgensam's all-RMT path is the more robust one on ESP32 specifically: it is what makes zero-gap TX and no-DE-pin operation possible, and it absorbs the `0xC0`→`0x01` aliasing that fast-replying monitors (7350A/8330A answering in 5–10 µs) cause when the transceiver receiver hasn't settled — a quirk HLM has no equivalent handling for.

---

## 4. Semantic conflicts on shared opcodes

### 4.1 Telemetry tags `0x43` / `0x45` are swapped in espgensam — settled by the captures

| | `0x43` | `0x45` |
|---|---|---|
| espgensam (`components/gensam/monitor.cpp:165-167`) | Woofer (LF) | Tweeter (HF) |
| HLM (spec §8.1) | HF-channel meter (conf 90%) | LF-channel meter (conf 70%) |

The captures settle it. Frames containing tag `0x46` come from the 7350A subwoofer (524 frames). In **every one of them** `43` reads `0x80` — the floor — while `45` ranges `0x80`–`0x94`. A subwoofer has no high-frequency driver, so the meter that is permanently floored is the HF one:

```
41 29 83 00 29 42 95 46 91 43 80 45 80 47 01 84 01 65   ← 7350A, 43 floored
41 13 83 00 25 42 00 46 FA 43 80 45 9D 47 01 84 01 6C   ← 7350A, 45 active, 43 floored
```

This matches HLM's independent swept-sine finding ("43 floors at/below ~1–2 kHz, rises with frequency"). **HLM is right; espgensam's labels are reversed.** Impact is cosmetic — espgensam takes `max()` across `43/44/45/46` for its single `output_db` — but the header documentation is wrong.

espgensam's `0x46` = subwoofer driver is confirmed and is new information relative to HLM, which never saw the tag.

### 4.2 Telemetry tag `0x84` is a device-class field, not a power state

HLM: "first byte `02` = ON / `03` = standby" (conf 30%). The captures show a perfect correlation with **device type**, not power state:

```
frames with tag 46 (subwoofer)        → 84 01     (524 frames)
frames with tag 81, no 46 (2-way)     → 84 02     (984 frames)
```

Both classes report `47 01` (active) throughout. HLM only ever saw `02` because it only ever had two 8320A. Its `03` observation is presumably a genuine standby marker, so the field may encode something like driver/channel count with a distinct standby value — worth flagging upstream rather than asserting.

**Resolved:** the GLM bus decoding guide defines `0x84` as a 16-bit processor load in per mille. The class correlation holds because each class's load falls in its own range. See reconciliation doc §2.2.

HLM also has a length bug here: the spec says `84` carries **2** bytes, but `hlm/src/HLM/hub_app.c:441-443` consumes 1. The `else if (tag == 0xB0) idx += 2;` branch at `:461` is a patch over the resulting misalignment (`B0` is the second byte of `84 02 B0`, mistaken for a tag). espgensam's generic `(tag & 0xF0) == 0x80 → skip 2` rule handles the whole `0x8x` family correctly.

### 4.3 The `0x06` / `0x07` reply prefix — an earlier `STATUS_STANDBY` reading was wrong

Both projects saw an extra byte between the `09` reply opcode and the `41` telemetry tag. Each handles a different value:

- **HLM** handles `0x06`: a settings-write/persistence marker, appearing ~0.6–1.1 s after config pushes, also seen bare as `09 06`. It does **not** handle `0x07` — such a frame falls through to `unknown_message()` and is dropped.
- **At the revision examined here**, espgensam handled `0x07` as `STATUS_STANDBY` (commit `968fdc4`), forcing `monitor.standby = true` and explicitly refusing to let tag `47` clear it (`components/gensam/monitor.cpp:193-199`). It had no notion of `0x06` (the scan-based TLV walk skipped it harmlessly, and a bare `[06]` was silently ignored).

The captures contain both, and they show the original observation was right while its conclusion was backwards. The marker does follow a wake — in all three captures it appears **~1.6 s after a `3A 03 7F` / `3A 03 01` burst** (1.57 s in `glm_startup`, 1.57 s in `glm_v5_no_wakeup`, 1.60 s in `glm_source_select`). But every marker-bearing frame carries **`47 01` — active**: the monitor has *finished* waking. Nor is the byte positional — 10 frames carry it leading, 5 carry it trailing (`41 29 … 47 01 84 01 6C 06`, all from the 7350A), and 2 consist of nothing else. No capture contains `47 02` anywhere.

```
[21:11:14.768] addr=0x01 cmd=0x09 payload=[07 41 13 83 00 25 42 00 46 FA 43 80 45 9D 47 01 84 01 6C]
[21:11:14.768] addr=0x01 cmd=0x09 payload=[07 41 14 81 00 23 83 00 25 42 B7 43 80 45 80 47 01 84 02 C3]
[21:11:14.768] addr=0x01 cmd=0x09 payload=[07 41 13 81 00 23 83 00 25 42 8F 43 80 45 80 47 01 84 02 C4]
```

(`captures/glm_v5_no_wakeup_capture.log:296,302,307`.) One second later the same monitors send byte-identical frames **without** the prefix. Meanwhile the Home Assistant System Power switch is `ON` and mute snooping is live — the system is not in standby.

At that revision, a `0x07` prefix on an active frame set `standby = true` and blocked tag `47 01` from clearing it. In this capture all three monitors got the prefix in the same round, which was enough for `evaluate_system_standby_()` to flip the whole system to standby in Home Assistant. A false standby was expensive rather than cosmetic: `set_volume_db` kept publishing to Home Assistant while transmitting nothing, so the slider moved and the speakers did not; the next `rediscover_monitors()` could escalate it into a genuine `send_standby()` on the wire.

**Current status:** espgensam treats `0x06` and `0x07` as transparent telemetry markers, not power states. Tag `0x47` with a valid operand is the authority for power state. The fix and regression tests are described in §7, item 1.

### 4.4 `0x40` — espgensam's model is a strict superset

Same opcode, same wire bytes, different field decomposition:

- HLM: `40 00 [Input] 02 [Counter]`, byte 3 "always `02`", byte 4 "always `00`" (conf 90%).
- espgensam: `[input_idx, source, pair_selector, aes3_channel]`, with `01`=analog / `02`=AES3 and channel `01`=A / `02`=B / `03`=A+B sum; subwoofers additionally get input index 1.

The captures show six distinct payloads, and only espgensam's model explains them:

```
[00 01 02 00]  input 0, analog          [01 01 01 00]  input 1, analog
[00 02 00 01]  input 0, AES3 ch A       [01 02 00 00]  input 1, AES3
[00 02 00 02]  input 0, AES3 ch B
[00 02 00 03]  input 0, AES3 ch A+B
```

HLM's "always `02`" is an artifact of only ever observing analog on one input, and its "Counter" byte is the AES3 sub-channel. Conversely HLM contributes a value espgensam lacks: **`03` = Automatic** input selection.

### 4.5 `0x3B` — the two readings are irreconcilable as stated

espgensam reads a big-endian crossover frequency in Hz and has `[00 5A]` = 90 Hz in the capture (13 occurrences, consistent). HLM only ever saw `3B 00 01` and labels it "sets full-band mode" (conf 60%). Under espgensam's model that is 1 Hz, which is meaningless.

The natural reconciliation: the field is an enum where small values are modes (`0x0001` = full-band / bass management off) and values in the 50–120 range are crossover frequencies in Hz. HLM's system has no subwoofer, espgensam's does. Neither has tested the other's case.

### 4.6 `0x2B` — bitfield vs. opcode table

- espgensam models a bitfield: bit 0 mute, bits 1-2 LED colour (`0`=green, `1`=red, `2`=off, `3`=yellow), bit 3 pulsing, bit 4 invert.
- HLM enumerates values: `03` = mute on, `04` = unmute / LED on, `08` = disable LED.

Live GLM LED toggles resolve these two values: `2B 08` turns LEDs off and `2B 04` turns them on, sent unicast to each monitor. Both were captured with mute clear; espgensam preserves bit 0 when constructing the command for a muted monitor. This is separate from the stored ISS LED setting (`3A 06`), which GLM writes only during Store Settings to Loudspeakers and follows with `15 33 00`.

The captures show per-monitor commands, not a group-level acknowledgement or readback. Accordingly, espgensam tracks observed LED state per discovered monitor and does not publish an authoritative system switch state until all discovered monitors have a known state; disagreement is represented as off. This is an integration policy, not a protocol-defined aggregate state.

### 4.7 `0x3A 03 02` — "System ON" or "standby prepare"?

HLM's table says `03 02` = System ON, `03 00` = System OFF — yet its own documented power-off sequence is `3A 03 02` ×2 followed by `3A 03 00` ×2, which is also exactly what espgensam sends for standby (calling `0x02` "standby phase 1 / prepare"). espgensam's reading is self-consistent; HLM's table contradicts its own observed sequence.

The wake sequences differ too, and the captures back espgensam:

```
OEM GLM (capture):   3A 03 7F, 3A 03 01   ×3     (21:53:35.638–.662)
espgensam:           3A 03 7F, 3A 03 01   ×3     ← identical
HLM:                 3A 03 7F, 3A 03 01, 3A 03 02
```

HLM's trailing `03 02` does not appear in OEM traffic. HLM also notes it deliberately sends **no** power command when leaving ISS idle, because doing so puts playing speakers into standby ~1.2 s later — a real behavioural hazard espgensam does not document.

### 4.8 `0x04` — espgensam's keep-alive reading is right

HLM calls `0x04` a "broadcast sync / config preamble, sent twice per speaker per push", and transmits it as `04 BC 84`. espgensam calls it `CMD_STAY_ONLINE`, sends it bare, and broadcasts it once per poll cycle to refresh address leases.

The captures support espgensam. In the source-select capture `0xFF 0x04` appears 127 times at a steady **1.2–1.8 s cadence**, always in the same position — immediately after the `0xFF 0x1F` volume broadcast and immediately before the round of `0x08` polls:

```
addr=0xFF cmd=0x1F payload=[00 67 9F]
addr=0xFF cmd=0x04 payload=[]      ← bare, no BC 84
addr=0x02 cmd=0x08 payload=[]
addr=0x03 cmd=0x08 payload=[]
```

That is a hub heartbeat, not a config preamble — and espgensam emits the identical volume → `04` → poll-round pattern. HLM saw only the config-burst occurrences (2 per burst, which is also what the other two captures show) and generalised from those. The `BC 84` payload does not appear in any OEM frame here.

---

## 5. Command coverage — HLM's documentation and subsequent espgensam support

At the revision used for the initial comparison, HLM's specification documented the following
commands and features:

| Op | What HLM has |
|---|---|
| `0x10 0E` | **20-band PEQ**, fully decoded: RBJ biquads at 48 kHz, five little-endian float32 `b0 b1 b2 a1 a2` with a1/a2 sign-inverted, type byte, plus the bypass shortcut vector `b0 = 1.0f`. This is the biggest single contribution. |
| `0x10 01` | Level compensation / boundaries: `00` max level restriction, `05` startup level, `09` unknown |
| `0x10 02` | Time-of-flight delay, 32-bit sample count @48 kHz |
| `0x05` | Signal generator: pink noise / sine, per-driver mute masks (`04` tweeter, `10` woofer, `14` both), 24-bit level, big-endian Hz with an 8-point calibration table |
| `0x15 33 00` | Commit to flash, plus the full ordered store sequence |
| `0x17 01` | Prepare for configuration |
| `0x3A 01/02/06` | ISS sleep delay (minutes), ISS sensitivity (High/Med/Low), LED on/off |
| `0x3D 00 00` | Input-select sync |
| `0x2D` | Input select / config preamble (conf 50%) |

The GLM bus decoding guide later renamed several of these opcodes: `0x15` is a service prefix
(`15 33 n` stores the user settings), `0x17 01` selects stored settings over the DIP switches,
`0x3D` is the bass-management bypass and `0x2D` has no effect. See
[`glm-bus-guide-reconciliation.md`](glm-bus-guide-reconciliation.md) §1.
| — | OEM per-speaker enumeration order; the 610 ms hub heartbeat and 2.6 s takeover rule; the standalone boot volume ramp |

Since that comparison, espgensam has implemented group and per-monitor PEQ, level and delay
commands and `0x17 01` before group DSP writes. It observes GLM's stored `0x3A 06` LED setting
and updates its diagnostic switch, but does not write that setting or commit it to monitor
flash. ISS sleep delay and sensitivity (`0x3A 01/02`) are now sent as runtime-only overrides,
without a store (reconciliation doc §3). It still does not implement signal-generator control,
monitor flash commits, or `0x2D` / `0x3D`. The `0x15 33 00` frame builder
(`make_store_user_params()`) exists but is not used.

The espgensam captures contain `0x05` with payload `[04 00 00 00 FF FF EA 00 01 90 00 02 DA]` —
byte-for-byte HLM's documented "stop generator / restore" frame, 14 times.

**espgensam knows, HLM does not:**

| Feature | Detail |
|---|---|
| `0x3B` as crossover Hz | `[00 5A]` = 90 Hz, with min/max/step 50/120/5 |
| `0x40` full model | AES3 sub-channel A/B/Sum, per-input indices, subwoofer second input |
| Telemetry `0x46` | Subwoofer driver meter, confirmed from the 7350A. `0x44` (midrange) is anticipated in code but appears in no capture — untested |
| `0x22` | Short system-info query (accepted as a reply type; first labelled a hardware-ID query) |
| `0x19` payload | Barcode query carries `{0x01}` — the only query with a payload |
| Device-info formats | Both `c-1;model-7350A;ver-…;hw-…;build-…` and space-delimited `7350A 1 0000 0106 3733` |
| Identity matching | Barcode/serial ↔ 3-byte hardware ID two-layer model, with substring matching for the optional `M` in `7350APM…` |
| External-master detection | Arbiter that classifies echo vs. reply vs. foreign traffic and yields the bus outright |
| New opcodes | `0x3C [00 00]`, `0x3E [00 00]`, `0x42 [00 00]` appear in OEM traffic and are in **neither** spec |

---

## 6. Architecture and behaviour

| | espgensam | HLM |
|---|---|---|
| Role | Sole master in normal operation; steps aside entirely if a real GLM adapter appears | Sole master; replaces the adapter |
| External master | `BusArbiter` classifies each frame as external-master / our-reply / loopback-echo (50 ms / 300 ms windows). On an external master it **stops transmitting altogether** and runs read-only, snooping volume, mute, power, monitor LED settings and addressing to keep its own state current; after 15 s of silence it resumes and rediscovers. It does not share the bus — there is no deferral or interleaving, only yield-and-watch | None; documents the OEM's own defer mechanism but does not implement it |
| Discovery trigger | Boot, cooldown expiry, HA button, GPIO button, **every wake from standby** (leases are lost on DSP reboot). Always wakes before discovery, restoring standby afterwards if the system was meant to be off | `FE` broadcast every tick in DISCOVERY (200 ms) and ACTIVE_POLLING (1080 ms); 4 missed polls marks a speaker inactive. Notes that a speaker recovered from standby needs no wake sequence at all — `FE` alone suffices |
| Reliability | 3 retries on ID assignment then assume-accepted; 3 attempts on device queries; duplicate-send for mute/crossover/identify after 250 µs | No per-frame retry; TX queue paced at one frame / 20 ms; double-send for state changes per OEM practice |
| Cadence | 300 µs turnaround, 20 ms between polls, 1 s poll cycle | 200 ms round-robin polls; state ticks at 610 / 1080 / 1220 ms |
| UI | Home Assistant via ESPHome native API: volume number, system power/mute/monitor-LED switches, per-monitor sensors, switches, buttons, selects, text sensors, status LED | SH1106 OLED + rotary encoder, local only; no network at all (HA extension is future work) |
| Extra modes | Passive snoop decoder | `HLM_HEX_DUMP` and `HLM_ANALYZER` builds — a standalone bus analyser over USB CDC |
| Hardware coverage | Tested on a 7350A sub + two 8330A two-ways. Model handling is string-based (`7…` = subwoofer, with W371 excluded), and nothing it sends depends on the DSP sample rate, so 48 kHz vs 96 kHz does not arise | 8320A only; 48 kHz DSP only, 96 kHz explicitly unsupported because the PEQ coefficients are rate-specific |
| Docs | Header banners + raw captures | Formal 313-line spec with per-item confidence percentages |

The two projects are close to complementary: espgensam is deeper on device topology, identity and on detecting and standing down for a foreign master; HLM is deeper on DSP configuration and has the better protocol document.

---

## 7. Open items, ranked

**Correctness — espgensam**

1. ~~**Re-examine `STATUS_STANDBY = 0x07`.**~~ **Done (§4.3).** `STATUS_STANDBY` is replaced by `is_telemetry_marker()`; `0x06`/`0x07` are stripped at either end without touching power state, tag `0x47` is the sole authority with its operand validated, and a marker-only payload updates nothing. Regression cover in `tests/test_parse_telemetry.cpp`, which fails six checks against the pre-fix code.
2. ~~**Swap the `0x43` / `0x45` labels.**~~ **Done.** `0x43` = HF/tweeter, `0x45` = LF/woofer, confirmed again on live hardware: with music playing, a 7350A reported `0x43` at the `0x80` floor in 164/164 frames while `0x45` swung `0x80`–`0xF1`. Behaviour is unchanged (`max()` across channels). `0x46` was relabelled at the same time — it is subwoofer-only, but has been seen both dominating while `0x45` floored and floored while `0x45` swung, so calling it "the subwoofer's output" overstates what is known.
3. ~~**Re-check the bus baud rate.**~~ **Done — measured at ~288,000 baud and updated in espgensam (§2).** HLM's generated rate is 2.88% high against that measurement.

**Protocol knowledge worth absorbing**

4. ~~Add `0x03` = Automatic to the `0x40` source enum.~~ **Closed — not the right action.** Automatic is a *standalone* setting, offered by GLM only when configuring what a speaker does on its own, not in live input selection; `0x40` is accordingly titled "Standalone Config Block / Input Routing" in HLM v12 and is step 6 of the flash-store sequence, immediately before `0x15 33 00`. Neither project has observed `03` on the wire — both list the same six payload variants, all `01`/`02` — so HLM's entry is inferred from the GLM UI and the `pair_selector`/`aes3_channel` bytes are unknown. It also could not be made to stick without a flash commit, which espgensam does not implement. Belongs to a standalone-settings feature (`0x40` + `0x3A 01/02/06` + `0x15`), not the input select. Capturing a GLM standalone store would settle both the encoding and that whole sequence.
5. ~~Document the `0x84` first byte as a device-class field.~~ **Explained by the GLM bus decoding guide:** `0x84` is a 16-bit processor load in per mille, and each class's load falls in its own range, so the high byte alone reads `01` = subwoofer, `02` = two-/three-way, `03` = transition or standby. Exposed as the diagnostic `Processor Load` sensor (reconciliation doc §2.2).
6. Record the newly-identified opcodes from HLM in `const.h` comments even if unimplemented — `0x05` generator, `0x10` DSP (PEQ / delay / level boundaries), `0x15` flash commit, `0x17` prepare-config, `0x3A 01/02/06` ISS settings, `0x3D` input sync — so the snoop decoder can label them instead of logging raw bytes. **Done:** `0x10` with its sub-commands, `0x15` (service), `0x17` (net settings), `0x3A 01/02`, `0x3C`, `0x3D`, `0x42`, `0x43`, `0x4B` and `0x53` are named in `const.h` after the GLM bus decoding guide (reconciliation doc §1). `0x05` is still unnamed.
7. Optional feature work, in rough order of value: ISS sleep delay / sensitivity via `3A 01/02` (**now implemented as runtime-only overrides**, reconciliation doc §3); startup and max level via `10 01`; PEQ snooping via `10 0E`; general flash-store sequencing. **Live monitor LED enable/disable via `2B 04/08` is implemented. The stored ISS LED setting via `3A 06` is observed and tracked, not written by the controller.** **PEQ, level and delay done** — see §7.1; the group preset feature transmits `17 01`, `10 02`, `10 01 00`, `10 0E` ×20, `3B` and `0x40` per speaker. Level (`10 01 00`) and delay (`10 02`) are also reachable per speaker outside a group, as Home Assistant number entities, sent without a preceding `17 01`: the captures show GLM omitting it for individual writes. The snoop decoder follows `10 01 00` and `10 02` back, so a change made in GLM is picked up rather than reverted at the next standby cycle; `10 01 09` is deliberately not decoded, since its constant `000000` payload would read as -130 dB and mute the speaker. Startup level (`10 01 05`), PEQ snooping, ISS sleep/sensitivity settings and general flash-store sequencing are not implemented. PEQ, level and crossover pushes are confirmed acoustically on an 8330A (`docs/glm-group-apply.md` §8).
8. ~~**Support `0x3B 00 01` as full band (§4.5).**~~ **Done.** Implemented on the mode-enum reading: `CROSSOVER_FULL_BAND = 0x0001` in `const.h`, anything else is Hz on the 50–120 / 5 Hz grid. The supporting evidence is now two independent sources rather than one: GLM's own setup file stores `CrossoverFrequency(Hz):1` and `Group_Crossover:1` for a sub-less group, alongside 80/85/90 for groups with a subwoofer. Group YAML spells it `crossover: full_band`, `sam_file:` / `sam2yaml.py` map GLM's `1` to it, and the per-monitor Home Assistant control became a select (`Full band`, `50 Hz` … `120 Hz`) because a number entity could only clamp `1` to 50 Hz. An AutoPhase angle in a full-band group yields no delay, since there is no crossover to realise it at. Measured on a sub-less 8330A: `3B 00 55` (85 Hz) high-passes it and `3B 00 01` leaves it full range (`docs/glm-group-apply.md` §8), which settles the sub-less half of the §8 experiment. The other half, `3B 00 01` sent to a system with the 7350A, is still untested.

### 7.1 Findings from the group-switch capture (2026-09-20)

A fourth capture was taken specifically to settle the DSP encodings: GLM switching a
calibrated 7350A + 2×8330A between the three groups of a GLM 5.2 setup file, with espgensam
listening only. It contains 37 distinct non-bypass PEQ coefficient sets, and all 37 are
reproduced to within **2.4e-7** — float32 epsilon near unity — by the implementation in
`components/gensam/biquad.cpp`. The items below are what that capture established; those
marked *upstream* are corrections to HLM's specification.

15. *(upstream)* **Shelving filters use the Q form, not the slope form.** Appendix A.3 gives
    `alpha = (sin w0 / 2)·sqrt((A + 1/A)(1/S − 1) + 2)` with S fixed at 1.0. Fitting designed
    coefficients against the captured ones instead gives `alpha = sin(w0) / (2Q)` with
    **Q = 0.3 for low shelves and Q = 0.5 for high shelves**. Both fit to four decimal places
    and one low-shelf band is bit-exact.

    **How strongly is each pinned?** An earlier revision of this document answered "the low
    shelf firmly, the high shelf weakly", reasoning that the two forms coincide at 0 dB and
    that every high shelf in the capture sits within 0.02 dB of flat. That reasoning was
    wrong, and it is worth recording why, because it is the intuitive answer.

    The two forms coincide at 0 dB in **response**, not in **coefficients**. Setting A = 1
    makes b equal a, so H(z) = 1 whatever `alpha` is — but `alpha` does not leave the
    normalised coefficients, which still carry it through `a0 = ... + 2·sqrt(A)·alpha`. The Q
    form gives `alpha = sin(w0)/(2Q)` and the slope form `alpha = (sin w0/2)·sqrt(2/S)` at
    A = 1, a fixed ratio of √2 apart for Q = 0.5, S = 1. A flat shelf therefore still reports
    its Q, in b1/a0 and a2/a0, as plainly as a boosted one.

    Fitting accordingly — frequency and gain held at the setup file's values, Q swept alone —
    gives, against the captured −0.0199986 dB / 14999 Hz high shelf:

    | model | max coefficient error |
    |---|---|
    | **Q = 0.5** | **1.2e-07** (float32 noise floor) |
    | Q = 0.48 | 2.0e-02 |
    | S = 1.0, i.e. Q = 0.7071 | 1.7e-01 |

    That brackets Q = 0.5 to **±2e-07**. The low shelf is independently confirmed over gains
    to −4.25 dB, fitting Q = 0.3 at 6e-08. Both constants are firm, and neither needs the
    large-gain capture §8 used to ask for.
16. *(upstream)* **`10 01 00` is per-device level compensation, not "Max Level Restriction".**
    Across the three groups its value tracked the setup file's `Level_Sensitivity` for the
    same physical subwoofer exactly: −1.9258 dB, −8.3783 dB and 0.0 dB. That setup's global
    volume limit is −20 dB, nowhere near any of them, and −8.38 dB is far too large for a
    plausible output ceiling. The scale is also **2²³ − 1, not 2²³**: 0 dB is transmitted as
    `7F FF FF`, and 2²³ does not fit the 24-bit field.
    `round(10^(dB/20) × 8388607)` reproduces every observed value exactly.
17. *(upstream)* **`10 02` is in 48 kHz samples on every device class**, which settles
    Appendix B item 8's 4× ambiguity for subwoofers. On a subwoofer the value is the AutoPhase
    angle expressed as a delay:
    `round(((phase mod 360) / 360) / crossover_Hz × 48000)` predicted all three groups exactly
    — 289, 267 and 67 samples for phases −165°, 180° and 45° at a 90 Hz crossover. The setup
    file's `Time-of-flight_Compensation` was zero throughout and is *not* the source, which is
    worth stating because it is the field the name suggests.
18. *(upstream)* **The bypass vector is used only at exactly 0 dB**, for every filter type.
    `generate_peq_48_payload()` treats `fabsf(gain_db) < 1e-4f` as flat, which would bypass
    bands GLM designs: the capture has a peaking band at −3.9e-14 dB and a shelf at
    −3.1e-05 dB, both transmitted as honestly computed near-identity sections rather than as
    `{1, 0, 0, 0, 0}`.
19. *(upstream)* **PEQ slot layout differs by device class.** A two-way monitor uses slots
    0–1 for low shelves, 2–3 for high shelves and 4–19 for peaking bands; a **subwoofer uses
    all twenty as peaking bands and has no shelves**. `on_peq_48()`'s index-to-name mapping
    assumes the two-way layout for every device, so it would decode a subwoofer's first four
    bands as shelves.
20. *(upstream)* **`generate_peq_48_payload()` hardcodes 48 kHz** (`glm_library.c:39`). With
    the subwoofer rate now confirmed on the wire rather than only in the config file, a
    subwoofer's bands designed through it land 4× high — a 40 Hz notch at 160 Hz.
21. *(upstream)* **The `.sam` `Input:` field is the complete routing enum** — 1 = AES3 A,
    2 = AES3 B, 3 = AES3 A+B sum, 4 = analog — which resolves the source and sub-channel bytes
    of `0x40` together. Verified across all three groups including one that runs the subwoofer
    on AES3 sum while both mains are analog, so **routing is per device, not per group**;
    `Group_Audio` is not the authority.
22. **Subwoofer PEQ design rate of 12 kHz is now confirmed on the wire**, not only inferred
    from the setup file (§4 of HLM v12 cites the config file). Two-way at 48 kHz likewise.

**Worth sending upstream to HLM**

8. `0x04` is a periodic hub keep-alive sent bare, not a `BC 84` config preamble (capture evidence, §4.8).
9. `0x40` byte 3 is a pair selector and byte 4 is the AES3 sub-channel, not a counter (six payload variants, §4.4).
10. `0x84` correlates with device class, and the `0x84` value length is 2 bytes — `hub_app.c:441` consumes 1, which the `0xB0` branch at `:461` is silently patching over.
11. HLM does not handle `0x07`; espgensam now treats both observed `0x06` and `0x07` values as transparent telemetry markers (§4.3).
12. Telemetry tags `0x44` (midrange) and `0x46` (subwoofer driver) exist on models other than two-ways.
13. Opcodes `0x3C`, `0x3E`, `0x42` (all with `[00 00]`) appear in OEM traffic and are in neither spec.
14. The OEM wake sequence is `3A 03 7F` / `3A 03 01` ×3 with no trailing `03 02`; and the `03 02` value appears at the head of the power-**off** sequence, which contradicts the "System ON" label.

---

## 8. Experiments that would settle the open items

These are hardware tests.

**Baud rate — done.** Implemented as `baud_sweep` in espgensam on the `baud-rate-sweep` branch (`components/gensam/baud_sweep.cpp`; kept off `main`, where it would cost ISR time and 2 KB of RAM for a one-off diagnostic) and measured at ~288,000 baud; result in §2. Worth noting for anyone repeating it: counting decode failures per candidate rate does **not** work, because the receiver tolerates the whole disputed span. Fit the bit period to the raw edge intervals instead, measuring edge-to-edge across a full RMT symbol so that asymmetric rise/fall on the line cancels. An independent confirmation with a scope — capturing the recurring `F0 1F 00 67 9F` heartbeat, whose byte values are known — would remove the dependence on the ESP32's own clock.

**`0x07` prefix — resolved.** Captures show marker-bearing replies with `47 01` (active); espgensam now strips `0x06`/`0x07` markers without changing power state. Regression coverage is in `tests/test_parse_telemetry.cpp`.

**`0x43` / `0x45`.** Play a 10 kHz tone and then a 40 Hz tone into one 8330A and watch which tag moves. Expect `43` on the tone and `45` on the bass.

**`0x2B` remaining LED encodings.** Live GLM toggles establish `2B 04` = LEDs on and `2B 08` = LEDs off. The effect of `2B 00`, `2B 02` and `2B 06`, including their interaction with mute and identify pulsing, remains unresolved.

**`0x3B`.** Send `3B 00 01` to an espgensam-managed system that has the 7350A in it and observe whether bass management disengages; and send `3B 00 5A` to a sub-less pair and observe whether anything changes. That distinguishes "mode enum with Hz overlay" from two unrelated encodings.

**High-shelf Q — withdrawn.** This asked for a large high-shelf gain, driven through the Sound Character Profiler, to settle `SHELF_Q_HIGH`. The existing capture already settles it; see §7.1 item 15 for why the near-flat shelves are sufficient and what the earlier reasoning got wrong. Noted in passing for anyone running an SCP capture for some other purpose: GLM clamps High Shelf 1 to +3 dB, so the ±6–10 dB this entry used to suggest is not reachable from that dialog.

The host regression test `tests/test_parse_telemetry.cpp` feeds captured payloads to
`parse_telemetry()` and checks marker-prefixed, bare and unprefixed forms, power state, levels
and temperature for both subwoofer and two-way frame shapes.

## 9. ISS standalone-store capture (2026-11-16)

### Isolated ESPGenSAM traffic trials (2026-11-16)

With the official GLM adapter disconnected, the passive baseline capture
`captures/iss-isolation-passive-20261116-1146.log` contained no RX bytes or TX frames.
The user stopped audio at approximately 11:51 and observed both monitors sleeping by
approximately 12:05 (sleep time uncertain). RX counters stayed unchanged throughout.

The telemetry-only experiment used the ignored local configuration
`iss-telemetry-local.yaml`: normal discovery/configuration once after boot, followed at
12:15:56 by a runtime listen-only gate. Every 500 ms it briefly opened the gate for one
`0x08` status poll, alternating the two discovered addresses. No wake, discovery, volume
or keepalive frames were sent after arming. This is diagnostic firmware, not a production
ISS implementation; normal entity commands cannot transmit while the gate is closed.

The capture `captures/iss-isolation-telemetry-20261116-1215.log` confirms audio meter
activity at 12:16:58--12:17:04, followed by silence and continuing active (`47 01`)
telemetry for more than fourteen minutes. The user reported neither monitor slept and
both LEDs blinked red before and after the audio. Replies continued, so expired addresses
did not invalidate this trial. At 12:31:27 RX counters showed zero framing, invalid-frame
or CRC errors since boot.

Thus telemetry polling alone, following one initial normal setup, was sufficient to
coincide with failure to sleep in this trial; repeated volume and keepalive broadcasts
are not required to reproduce it. The red blinking condition remains unexplained and is
a confounder, not an ISS sleep indicator. No conclusion about its cause or individual
effects of volume/keepalive is established. Passive firmware was restored and verified
afterward; the logger was stopped. Further trials should first resolve the LED condition.

### Online ISS toggle: preliminary observation

A separate passive capture, `captures/glm-online-iss-20261116-0917.log`, records GLM
initialization followed by the user unchecking and rechecking **Enable ISS with GLM Online**.
No distinct ISS-setting write or flash commit was decoded for either toggle; ordinary
discovery/lease refresh, volume broadcasts and telemetry polls continued. ESPGenSAM emitted
no TX records. The capture has framing/invalid-frame errors, so absence of a decoded command
is not proof that no command was sent.

The test waited about ten seconds after each toggle, shorter than the stored ten-minute
ISS delay. It therefore does not establish sleep behavior or the toggle's implementation.
GLM may implement an application-side idle policy; that remains a hypothesis. A sustained
no-audio comparison with the toggle enabled and disabled is needed before implementing an
equivalent policy. The original checked state was restored. ESPGenSAM remains listen-only;
the recording was stopped after this test.

### Standalone settings

An additional audio-wake test was captured in
`captures/glm-iss-audio-wake-20261116-1103.log`. The user reported the monitors initially
in ISS standby, with the official GLM adapter disconnected. On audio playback they woke
quickly and their LEDs turned green. ESPGenSAM remained listen-only. The log contains no
raw RX records, decoded RX frames or TX records; receive-character and burst counters were
identical at 11:03:12 and 11:07:12. Thus no unsolicited RS-485 wake announcement was observed
in this test. Audio can wake these monitors independently, but a bus-silent controller cannot
rely on that wake producing a notification. This does not establish whether periodic probes
would interfere with ISS, nor prove that every model behaves identically.

Official GLM's **Store Settings to Loudspeakers** dialog was captured with ESPGenSAM in
verified listen-only mode. The raw capture was discarded after analysis; only the summarized
observations below are retained. All settings below were written to both monitor addresses,
`0x0C` and `0x0D`, followed by `cmd=0x15 payload=[33 00]` on each device. Only one ISS
setting was changed per trial; startup level, maximum level restriction, LED setting and input
selection were left unchanged.

| UI setting stored | `0x3A` payload | Interpretation |
|---|---|---|
| Sleep delay 10 min | `01 00 00 00 0A` | Timeout value 10 |
| Sleep delay 30 min | `01 00 00 00 1E` | Timeout value 30 |
| Sleep delay restored to 10 min | `01 00 00 00 0A` | Reproduces baseline |
| Sensitivity High, repeated baseline | `02 FF FF FF B5` | Signed value -75 |
| Sensitivity Medium | `02 FF FF FF BC` | Signed value -68 |
| Sensitivity Low | `02 FF FF FF C3` | Signed value -61 |
| Sensitivity restored to High | `02 FF FF FF B5` | Reproduces baseline |

This establishes `0x3A 01` as a four-byte big-endian timeout in minutes for the tested
10/30-minute choices, and `0x3A 02` as the sensitivity setting with the three signed
32-bit big-endian values above. These are not simple High/Medium/Low enum indices.
The physical units of the sensitivity numbers have not been independently established.
The UI also offered sleep delays of 2 min, 1 h, 2 h, 3 h and 4 h; those choices were not
stored during this test. No disabled-state encoding was established here; the GLM bus
decoding guide later gave Never as `FF FF FF FF` and the same sensitivity values in dBu
(reconciliation doc §3).

Every store also carried `0x3A payload=[06 00]` with **LEDs On**, and input-selection
frames `0x40 payload=[00 03 02 01]` / `[00 03 02 02]` with **Automatic** selected.
Those are baseline observations, not controlled LED or automatic-routing experiments.
The group reapplication after the first store included
`0x10 payload=[02 00 00 00 29]` to `0x0D`, consistent with the group's 0.85 ms ToF value
being rounded to 41 samples at 48 kHz. A controlled ToF sweep is still needed to establish
the general rounding rule and any interaction with AutoPhase.

The original settings were restored and observed on the wire: 10 min, High, LEDs On.
No ESPGenSAM `TX ->` records occurred. The log contains some invalid-CRC frames elsewhere;
the listed ISS-setting records and flash commits were decoded without invalid-CRC markers
on both addresses. This verifies GLM's transmitted settings and commit sequence, not
readback after a power cycle or measured sleep/wake behavior. The standalone LED setting was captured in listen-only mode with both values: each monitor
received `0x3A 06 01` when disabled and `0x3A 06 00` when enabled, followed by `0x15 33 00`.
See `captures/glm-led-toggle-20261116-1528.log`.
