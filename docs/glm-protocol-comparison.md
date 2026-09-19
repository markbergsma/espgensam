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
| Opcodes | `02` assign ID · `04` broadcast sync · `08` telemetry poll · `09` reply · `19` serial · `1F` volume · `2B` mute/LED · `39` info · `3A` power · `3B` bass mgmt · `40` input select · `FE` discovery |
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
| Configured | `281250` (`components/gensam/const.h:13`) | `296000` (`hlm/src/STM32F407/Core/Src/main.c:497`, `hlm/src/ESP32Common/rs485_9n2.c`) |
| Actually generated | 281,250 exactly (RMT, Q16 fixed-point accumulator, zero drift) | **296,296** on both ports — ESP32 `ticks_per_bit = 80e6/296000 = 270` → 296,296; STM32F103 USART1 @72 MHz quantises USARTDIV to 15.1875 → 296,296 |

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

The measured 287,773 bps is **0.079% from 288,000**, and 288,000 fits 16–21× better than either implementation's rate. The prediction holds: espgensam runs 2.27% low, HLM 2.88% high, and both are inside a normal UART's budget. One caveat — the measurement is relative to the ESP32's own 10 MHz RMT clock, so a small crystal offset could account for that last 0.08%; given 6 × 48 kHz is exact, 288,000 is almost certainly the intended figure.

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

HLM also has a length bug here: the spec says `84` carries **2** bytes, but `hlm/src/HLM/hub_app.c:441-443` consumes 1. The `else if (tag == 0xB0) idx += 2;` branch at `:461` is a patch over the resulting misalignment (`B0` is the second byte of `84 02 B0`, mistaken for a tag). espgensam's generic `(tag & 0xF0) == 0x80 → skip 2` rule handles the whole `0x8x` family correctly.

### 4.3 The `0x06` / `0x07` reply prefix — espgensam's `STATUS_STANDBY` reading looks wrong

Both projects saw an extra byte between the `09` reply opcode and the `41` telemetry tag. Each handles a different value:

- **HLM** handles `0x06`: a settings-write/persistence marker, appearing ~0.6–1.1 s after config pushes, also seen bare as `09 06`. It does **not** handle `0x07` — such a frame falls through to `unknown_message()` and is dropped.
- **espgensam** handles `0x07` as `STATUS_STANDBY` (commit `968fdc4`), forcing `monitor.standby = true` and explicitly refusing to let tag `47` clear it (`components/gensam/monitor.cpp:193-199`). It has no notion of `0x06` (the scan-based TLV walk skips it harmlessly, and a bare `[06]` is silently ignored).

The captures contain both, and they argue against the standby reading. All three `0x07`-prefixed frames carry **`47 01` — active** — and arrive in a single poll round, ~0.4 s after a configuration burst (`3D`, `2B`, `19` queries), which is exactly the context HLM documents for `0x06`:

```
[21:11:14.768] addr=0x01 cmd=0x09 payload=[07 41 13 83 00 25 42 00 46 FA 43 80 45 9D 47 01 84 01 6C]
[21:11:14.768] addr=0x01 cmd=0x09 payload=[07 41 14 81 00 23 83 00 25 42 B7 43 80 45 80 47 01 84 02 C3]
[21:11:14.768] addr=0x01 cmd=0x09 payload=[07 41 13 81 00 23 83 00 25 42 8F 43 80 45 80 47 01 84 02 C4]
```

(`captures/glm_v5_no_wakeup_capture.log:296,302,307`.) One second later the same monitors send byte-identical frames **without** the prefix. Meanwhile the media_player entity is `ON` and mute snooping is live — the system is not in standby.

Consequence: a `0x07` prefix on an active frame sets `standby = true` and blocks tag `47 01` from clearing it. In this capture all three monitors got the prefix in the same round, which is enough for `evaluate_system_standby_()` to flip the whole system to standby in Home Assistant. This looks like a live false-standby bug, and `0x06`/`0x07` are most likely the same field (a busy/sequence marker) with two values.

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

They collide on the colour bits: espgensam's model reads `04` as "unmuted, LED **off**"; HLM observed `04` as "LED **on**". `08` is "pulsing with colour green" under espgensam and "LED off" under HLM. The captures only contain `2B 04` (56×), so they do not adjudicate. espgensam's bitfield is the more predictive hypothesis; HLM's labels are direct observation. Testable in minutes on real hardware.

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

## 5. Command coverage — what each knows that the other does not

**HLM knows, espgensam does not** (all of these are visible in the espgensam captures but unimplemented):

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
| — | OEM per-speaker enumeration order; the 610 ms hub heartbeat and 2.6 s takeover rule; the standalone boot volume ramp |

The espgensam captures contain `0x05` with payload `[04 00 00 00 FF FF EA 00 01 90 00 02 DA]` — byte-for-byte HLM's documented "stop generator / restore" frame, 14 times.

**espgensam knows, HLM does not:**

| Feature | Detail |
|---|---|
| `0x3B` as crossover Hz | `[00 5A]` = 90 Hz, with min/max/step 50/120/5 |
| `0x40` full model | AES3 sub-channel A/B/Sum, per-input indices, subwoofer second input |
| Telemetry `0x46` | Subwoofer driver meter, confirmed from the 7350A. `0x44` (midrange) is anticipated in code but appears in no capture — untested |
| `0x22` | Hardware-ID query opcode (accepted as a reply type) |
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
| External master | `BusArbiter` classifies each frame as external-master / our-reply / loopback-echo (50 ms / 300 ms windows). On an external master it **stops transmitting altogether** and runs read-only, snooping volume, mute, power and addressing to keep its own state current; after 15 s of silence it resumes and rediscovers. It does not share the bus — there is no deferral or interleaving, only yield-and-watch | None; documents the OEM's own defer mechanism but does not implement it |
| Discovery trigger | Boot, cooldown expiry, HA button, GPIO button, **every wake from standby** (leases are lost on DSP reboot). Always wakes before discovery, restoring standby afterwards if the system was meant to be off | `FE` broadcast every tick in DISCOVERY (200 ms) and ACTIVE_POLLING (1080 ms); 4 missed polls marks a speaker inactive. Notes that a speaker recovered from standby needs no wake sequence at all — `FE` alone suffices |
| Reliability | 3 retries on ID assignment then assume-accepted; 3 attempts on device queries; duplicate-send for mute/crossover/identify after 250 µs | No per-frame retry; TX queue paced at one frame / 20 ms; double-send for state changes per OEM practice |
| Cadence | 300 µs turnaround, 20 ms between polls, 1 s poll cycle | 200 ms round-robin polls; state ticks at 610 / 1080 / 1220 ms |
| UI | Home Assistant via ESPHome native API: media_player, per-monitor sensors, switches, buttons, selects, text sensors, status LED | SH1106 OLED + rotary encoder, local only; no network at all (HA extension is future work) |
| Extra modes | Passive snoop decoder | `HLM_HEX_DUMP` and `HLM_ANALYZER` builds — a standalone bus analyser over USB CDC |
| Hardware coverage | Tested on a 7350A sub + two 8330A two-ways. Model handling is string-based (`7…` = subwoofer, with W371 excluded), and nothing it sends depends on the DSP sample rate, so 48 kHz vs 96 kHz does not arise | 8320A only; 48 kHz DSP only, 96 kHz explicitly unsupported because the PEQ coefficients are rate-specific |
| Docs | Header banners + raw captures | Formal 313-line spec with per-item confidence percentages |

The two projects are close to complementary: espgensam is deeper on device topology, identity and on detecting and standing down for a foreign master; HLM is deeper on DSP configuration and has the better protocol document.

---

## 7. Open items, ranked

**Correctness — espgensam**

1. **Re-examine `STATUS_STANDBY = 0x07`** (`components/gensam/const.h:85`, `monitor.cpp:147-154`, `:193-199`). Evidence in `captures/glm_v5_no_wakeup_capture.log:296,302,307` shows a `0x07` prefix alongside `47 01` (active) right after a config push. Likely fix: treat `0x06` and `0x07` identically as a busy/sequence marker, strip it, and let tag `47` be the sole authority on standby. Keep the bare-`[07]`/`[06]` single-byte case as "no telemetry this round" rather than "standby".
2. **Swap the `0x43` / `0x45` labels** (`components/gensam/monitor.cpp:165-167`, `monitor.h` banner): `0x43` = HF/tweeter, `0x45` = LF/woofer. Confirmed by 524 subwoofer frames where `43` is permanently floored, and independently by HLM's swept-sine test. Behaviour is unchanged (`max()` across drivers) — this is a documentation/label fix.
3. ~~**Re-check the bus baud rate.**~~ **Done — measured at ~288,000 baud (§2).** Remaining decision: whether to change `baud_rate` from 281,250 to 288,000. Nothing is broken at 281,250 (the receiver tolerates the offset, and monitors evidently accept our transmissions), so this is a correctness-of-intent change rather than a bug fix. HLM should hear about it too — it is 2.88% high on the other side.

**Protocol knowledge worth absorbing**

4. Add `0x03` = Automatic to the `0x40` source enum.
5. Document the `0x84` first byte as a device-class field (`01` = subwoofer, `02` = two-way, `03` reported by HLM in standby) rather than leaving the whole `0x8x` family as "skip 2".
6. Record the newly-identified opcodes from HLM in `const.h` comments even if unimplemented — `0x05` generator, `0x10` DSP (PEQ / delay / level boundaries), `0x15` flash commit, `0x17` prepare-config, `0x3A 01/02/06` ISS settings, `0x3D` input sync — so the snoop decoder can label them instead of logging raw bytes.
7. Optional feature work, in rough order of value: ISS sleep delay / sensitivity / LED via `3A 01/02/06`; startup and max level via `10 01`; time-of-flight delay via `10 02`; PEQ via `10 0E`. Anything persisted needs `15 33 00`.

**Worth sending upstream to HLM**

8. `0x04` is a periodic hub keep-alive sent bare, not a `BC 84` config preamble (capture evidence, §4.8).
9. `0x40` byte 3 is a pair selector and byte 4 is the AES3 sub-channel, not a counter (six payload variants, §4.4).
10. `0x84` correlates with device class, and the `0x84` value length is 2 bytes — `hub_app.c:441` consumes 1, which the `0xB0` branch at `:461` is silently patching over.
11. `0x07` exists alongside `0x06` in the reply-prefix slot and is currently dropped as `unknown_message`.
12. Telemetry tags `0x44` (midrange) and `0x46` (subwoofer driver) exist on models other than two-ways.
13. Opcodes `0x3C`, `0x3E`, `0x42` (all with `[00 00]`) appear in OEM traffic and are in neither spec.
14. The OEM wake sequence is `3A 03 7F` / `3A 03 01` ×3 with no trailing `03 02`; and the `03 02` value appears at the head of the power-**off** sequence, which contradicts the "System ON" label.

---

## 8. Experiments that would settle the open items

These are hardware tests.

**Baud rate — done.** Implemented as `baud_sweep` in espgensam (see `components/gensam/baud_sweep.cpp`) and measured at ~288,000 baud; result in §2. Worth noting for anyone repeating it: counting decode failures per candidate rate does **not** work, because the receiver tolerates the whole disputed span. Fit the bit period to the raw edge intervals instead, measuring edge-to-edge across a full RMT symbol so that asymmetric rise/fall on the line cancels. An independent confirmation with a scope — capturing the recurring `F0 1F 00 67 9F` heartbeat, whose byte values are known — would remove the dependence on the ESP32's own clock.

**`0x07` prefix.** Re-run a capture while pushing a configuration change from GLM (source select or crossover) with the system definitely on, and confirm `0x07`-prefixed frames carry `47 01`. Then verify the HA `media_player` does not flip to standby during that burst.

**`0x43` / `0x45`.** Play a 10 kHz tone and then a 40 Hz tone into one 8330A and watch which tag moves. Expect `43` on the tone and `45` on the bass.

**`0x2B` LED bits.** Send `2B 00`, `2B 02`, `2B 04`, `2B 06`, `2B 08` to one monitor and record the front LED colour and steady/pulsing behaviour. This settles the espgensam bitfield vs. HLM enum question in one pass.

**`0x3B`.** Send `3B 00 01` to an espgensam-managed system that has the 7350A in it and observe whether bass management disengages; and send `3B 00 5A` to a sub-less pair and observe whether anything changes. That distinguishes "mode enum with Hz overlay" from two unrelated encodings.

Suggested regression coverage once fixes land: a table-driven unit test over the real capture payloads in `captures/` feeding `parse_telemetry()`, asserting standby state, per-driver levels and temperature for the `06`-prefixed, `07`-prefixed, bare and unprefixed forms, and for both the sub (`46`/`84 01`) and two-way (`81`/`84 02`) frame shapes.
