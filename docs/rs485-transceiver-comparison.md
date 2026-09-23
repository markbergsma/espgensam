# RS-485 bus measurements

Controlled measurements on a live three-speaker GLM bus, 2026-09-22.

Every run here was taken with `tools/capture_run.py`, which marks the counters *after* discovery
completes, accumulates a fixed number of frames, and aborts if the device reboots or yields
mid-sample. One variable changes per run and the configuration is recorded in the label.

For why the code is shaped the way it is, see [`rs485.h`](../components/gensam/rs485.h) and
[`uart9bit.h`](../components/gensam/uart9bit.h).

## Setup

The table below is the Waveshare, and applies to sections 1 and 2. Sections 3 to 5 involve other
boards or other bus configurations and each states its own.

| | |
|---|---|
| Board | Waveshare ESP32-S3-RS485-CAN — SP3485EN, `/RE` tied to `DE` on GPIO21, galvanically isolated |
| Pins | TX GPIO17, RX GPIO18, direction GPIO21 |
| Bus | Three speakers (7350A + 2× 8330A), 288,000 baud |
| On the bus | **One controller only.** No GLM adapter, no second board, no oscilloscope. |
| Ground | RJ45 pin 8 unconnected — the board's RS-485 terminal is A/B only |
| Mode | Bus master (`listen_only: false`), `poll_interval: 1s` |
| Firmware | `tx_echoes_rx: false`, direction line released in the RMT TX-done ISR |
| Sample | ≥4,000 frames per run, marked after discovery |

Nothing on this bus is shared with any earlier measurement session; runs are only compared with
each other.

## 1. Bus termination

The board's 120 Ω terminator is on a jumper, so it can be pulled without touching anything else.

| Label | Terminators | chars | frames | framing | stop2 | crc | invalid | `C0` | `start rej` |
|---|---|---|---|---|---|---|---|---|---|
| term-out, no adapter, no scope | 0 | 94,700 | 4,000 | 0 | 0 | 0 | 0 | **2,850** | 1,799 |
| term-in, no adapter, no scope | 1 (120 Ω) | 100,793 | 4,257 | 0 | 0 | 0 | 0 | **37** | 27 |

As rates:

| | unterminated | terminated | change |
|---|---|---|---|
| `C0 alias` / frames | 71.25% | **0.87%** | 82× better |
| `start rej` / chars | 1.90% | **0.027%** | 71× better |
| framing, stop2, crc, invalid | 0 | 0 | — |

**Terminate the bus.** This is the single largest effect measured, and it moves both counters in
the same direction.

Note what did *not* change: framing errors, stop-bit rescues, CRC failures and invalid frames were
zero in both runs. Unterminated, the bus still delivered every frame correctly — but only because
the `0xC0` workaround was repairing 71% of them.

### Why: the alias is a settling-time effect

After the direction line is released the bus is undriven and must coast to its biased idle mark
before the monitor asserts, 5–10 µs later. That coast is an RC set by the resistance across A/B
against cable capacitance (~500 pF on a short run):

| Terminators | R across A/B | τ |
|---|---|---|
| 1 | 120 Ω | 60 ns |
| 0 | ~9.4 kΩ (the fail-safe bias pair alone) | **4.7 µs** |

Unterminated, τ exceeds a bit time (3.47 µs) and is comparable to the reply delay, so the
monitor's start bit arrives before a valid mark has been established and its falling edge is not a
real edge. The decoder locks one bit late and the address decodes as `0xC0` — exactly the "too
shallow, not too narrow" mechanism described in `docs/crc-error-investigation.md` (branch
`crc-investigation`), and the reason the fault is a *turnaround* property.

### The two boards fail differently when unterminated

Observed, not captured:

| Board | Unterminated behaviour |
|---|---|
| Waveshare | degraded but functional — 71% aliases, still zero CRC errors, every frame delivered |
| M5Stack Atomic RS485 Base | **no communication at all** |

Restoring a terminator revives the Atomic base; a GLM adapter's TERMINATOR port is sufficient,
with the adapter otherwise idle.

Counters from the Atomic base unterminated, master mode, over several minutes including a manual
rediscovery:

```
Stats: 24 chars (24 addr, 0 data), 24 bursts | rx: 0 start rej, 0 stop2, 0 framing errs
     | frames: 0 ok, 18 invalid, 0 crc errs, 24 C0 alias [Monitors: 0]
```

Every character received is address-marked and every one is a `C0` alias. `C0'` is specifically a
mangled `0x01'` (see frame.cpp), and only monitors address the host — so **the monitors are
replying**, and our transmitted queries are reaching them intact. What fails is reception: the
first character of each reply arrives mangled and **not one subsequent character ever decodes**
(`0 data` across the whole session). `start rej`, `framing` and `stop2` are all zero, so this is
not noise; the characters are well formed.

It is the same lost-start-edge mechanism as the unterminated Waveshare, but qualitatively worse.
There, 71% of frames needed the fixup yet the remainder of each frame decoded and every frame was
delivered. Here the reply dies after its first character.

**Why the severity differs is not established.** Both boards appear to carry 4.7 kΩ fail-safe
bias, so a missing bias network does not explain it, and the monitors' replies show our
transmissions are fine. Candidates, none tested — differences in receiver threshold or hysteresis,
in bias topology, or in true fail-safe (open-line) behaviour between the two front ends.

Not pursued further: the operating rule below resolves it in practice. The consequence that does
matter is that **any board comparison must give both boards the same termination**, or it
measures the network rather than the board.

### Operating rule

Exactly **one** terminator at the controller end of the bus:

- GLM adapter attached — it carries one, passive, present even when the adapter is unpowered →
  **pull the Waveshare jumper**.
- Waveshare alone → **leave the jumper in**.

Whether the far end of the speaker chain terminates internally has not been established, so
"one at each end" has not been tested.

## 2. Direction-line release path

Does releasing the direction line from the RMT TX-done ISR measurably beat releasing it from task
context after `rmt_tx_wait_all_done()` plus 5 µs, as the pre-refactor code did? On this board
`/RE` is tied to `DE`, so a late release is a receive blackout landing on the reply's start bit,
and `docs/crc-error-investigation.md` predicted the task-context path would lose replies.

Software-only A/B, termination and everything else pinned:

| Label | chars | frames | bursts | framing | stop2 | crc | invalid | `C0` | `start rej` |
|---|---|---|---|---|---|---|---|---|---|
| release in TX-done ISR | 100,793 | 4,257 | 4,257 | **0** | **0** | **0** | **0** | 37 | 27 |
| release in task context | 90,668 | 4,001 | 4,212 | 133 | 61 | **28** | 2 | 132 | 49 |

**The ISR release is what makes the receive path error-free.** Moving it back to task context
costs 28 CRC-failed frames per 4,000 — replies lost outright, not repaired — plus 133 framing
errors and 61 stop-bit rescues where there were none. The `0xC0` alias rate rises 3.8×. At a
0.70% CRC rate, observing zero in the ISR run has probability ~1e-12.

### The burst count is the mechanism, visible directly

| | bursts | frames |
|---|---|---|
| ISR release | 4,257 | 4,257 |
| task-context release | 4,212 | 4,001 |

With the ISR release, bursts and frames match **exactly**: every RMT burst yields precisely one
frame. With the task-context release there are 211 more bursts than frames — replies arriving
split across multiple bursts, which is what a receiver being unblanked partway through a reply
produces. The blackout is not inferred from error counts; it is visible in how reception
fragments.

`bursts == frames` is worth watching as a health indicator in its own right.

### Note on `slow rel`

`slow rel` / `tx_overshoot_max_us` are **not comparable between these two builds** — the
task-context variant necessarily includes the wait and the 5 µs delay in the measured interval
(1,356 events vs 420). The comparison rests on `crc`, `framing` and `bursts`.

## 3. Board comparison

The question the work started from. Both boards as bus master, a single 120 Ω terminator at the
controller end, no GLM adapter and no other controller on the bus, one board at a time.

| Label | chars | frames | bursts | framing | stop2 | crc | invalid | `C0` | `start rej` |
|---|---|---|---|---|---|---|---|---|---|
| AtomS3 + Atomic RS485 Base | 76,240 | 4,014 | 11,420 | 557 | 44 | **64** | 83 | 117 | 0 |
| Waveshare ESP32-S3-RS485-CAN | 100,793 | 4,257 | 4,257 | **0** | **0** | **0** | **0** | 37 | 27 |

As rates:

| | AtomS3 + Base | Waveshare |
|---|---|---|
| `crc errs` / frames | 1.594% | **0%** |
| `framing errs` / chars | 0.731% | **0%** |
| `C0 alias` / frames | 2.915% | 0.869% |
| `invalid` / frames | 2.068% | **0%** |
| bursts : frames | **2.84 : 1** | **1.00 : 1** |

64 frames lost outright against none; at the AtomS3's rate, zero CRC errors in 4,257 frames has
probability of order 1e-30. The burst:frame ratio reproduces the fragmentation seen in every
earlier AtomS3 capture.

Note the AtomS3 also reproduces the original `crc-investigation` finding: that baseline measured
3.081% CRC on a different day in a session later discarded as confounded, and this controlled run
lands in the same family. The fault is real and repeatable.

### Configuration

| | AtomS3 + Atomic RS485 Base | Waveshare |
|---|---|---|
| Termination | external 120 Ω across A/B | onboard jumper fitted |
| Bus GND | connected | none — the terminal block has no ground pin |
| Direction control | auto-direction, no GPIO | GPIO21, released in the TX-done ISR |
| Raw log | `run-20260922-230742-atoms3-...` | `run-20260922-210520-term-in-...` |

Grounding necessarily differs: one board is non-isolated and needs its reference, the other is
isolated and exposes no ground terminal. Each is wired the only way it can be.

### Caveats

- **The AtomS3's error rate is not stationary.** Its first ~1,500 frames tracked 2.96% CRC and the
  remaining ~2,500 ran at 0.79%, while framing errors held near 0.8% throughout. The pooled figure
  is what the table reports, but the board is less consistent than a single rate suggests.
- **`slow rel` cannot be compared between these boards.** It is only computed when a direction pin
  is asserted, so it is structurally zero on the AtomS3 rather than measured.
- Part of the Waveshare's margin is the ISR direction release, independently worth 28 CRC errors
  per 4,000 frames (§2). A board without a direction line cannot benefit from it. That is the
  mechanism, not a confound.

## 4. M5Stack Isolated RS485 Unit (RS485-ISO)

The AtomS3 paired with an isolated front end and a third direction topology — CA-IS3082W, `DI`
grounded, the direction line driven by inverted TX bit by bit. Intended to separate "auto-direction
as an approach" from "the Atomic Base's particular circuit". Same bus as §3: 120 Ω terminator, no
GLM adapter, master mode.

### `tx_echoes_rx`, measured both ways

| | `false` (4,005 frames) | `true` (3,449 frames) |
|---|---|---|
| `start rej` / chars | 113,895 (**106%**) | **33 (0.036%)** |
| `echo cancelled` | 0 | 0 |
| `crc errs` / frames | 0.375% | 0.377% |
| `C0 alias` / frames | 2.971% | 2.696% |
| `framing errs` / chars | 0.894% | 0.938% |
| bursts : frames | 2.93 : 1 | 3.01 : 1 |

Because `DE` is inverted TX bit by bit, every *mark* bit releases the driver and re-enables the
receiver mid-transmission, so our own traffic reaches RX as chatter. With the skip disarmed the
decoder rejected more start edges than it decoded characters. Arming it removes that completely
and changes nothing else: **`true` is correct here, but it was cosmetic.** The board's error rates
were never caused by decoding its own transmission.

`echo cancelled` stayed at 0 throughout, so the leak burst reliably precedes any reply and the
skip is safe on this board.

### Receive path, isolated: listen-only with GLM driving

The CA-IS3082W is specified to 500 kbps, and its driver enable/disable time (3 µs typ, 5 µs max)
is not smaller than a 3.47 µs bit at 288 kbaud — which on a per-bit `DE` topology looked like it
might make the part unusable at this rate. Testing that means taking our own transmissions off the
wire: in `listen_only` the TX line rests at mark, `DE` stays deasserted, the receiver is
continuously enabled, and nothing toggles per bit. The GLM adapter drove the bus; monitors were
learned by snooping.

| | master (`tx_echoes_rx: true`) | **listen-only** |
|---|---|---|
| chars / frames | 91,123 / 3,449 | 38,803 / 4,101 |
| bursts : frames | 3.01 : 1 | **0.93 : 1** |
| `crc errs` | 0.377% | **0.024%** (1) |
| `framing errs` | 0.938% | **0.330%** |
| `C0 alias` | 2.696% | **0.634%** |
| `start rej` | 33 | **0** |

**The receive path is not the problem.** With nothing of ours on the wire, CRC essentially
vanishes, framing improves 3×, aliases 4× and start rejects go to zero. The burst ratio inverts:
0.93 per frame, below 1.0 because GLM packs several frames into a burst, against 3.01 when we
transmit. The transceiver decodes 288 kbaud perfectly well.

So the switching-time concern does not apply to reception — it cannot, since `DE` never toggles
while listening. Whether it applies to *transmission* is untested, and that is now the only place
this board's problems can originate: its own driving, through the per-bit `DE` topology.

Two limits on this run. The residual 0.330% framing is not zero, and since this measures GLM's
transmissions it could be the adapter's driving rather than our receiving. And a passive run
cannot reproduce the bus losses below, which happened over hours in master mode.

### The finding that actually matters

**It loses the whole bus, repeatedly, in normal use.** One controlled run aborted when all three
monitors timed out at ~3,500 frames; across three hours of real listening it happened several
times. It recovers unaided, but recovery re-runs device configuration, and that path silences
output deliberately to avoid pops — so each event is **a few seconds of audible silence**. An
earlier attempt also enumerated a phantom monitor from a corrupted discovery reply.

Weeks of use on the Atomic RS485 Base have never produced this.

### Why the counters did not predict it

This board has the better frame-level integrity of the two auto-direction front ends — 0.377% CRC
against the Atomic Base's 1.594% — and is nonetheless the worse board to live with. Frame-level
loss is absorbed by retries and is inaudible. Enumeration-level failure is not: it drops the
registry, triggers rediscovery, and interrupts audio.

**Per-frame error rates are not a proxy for user-visible reliability, and every metric in this
document is a per-frame rate.** A board should not be recommended on the strength of §1–§4 alone.
The acceptance test is hours of real use under load, and the metric is bus losses per hour, not
CRC percent. `grep -c "operational status changed to: Offline"` over a long `esphome logs` capture
counts them.

### Untested on the other boards

Three hours of playback with amplifiers driving is a condition neither §3 board has been measured
under; the longest Waveshare run here is ~25 minutes with the system idle. That matters
specifically because **the Waveshare is also galvanically isolated with no ground reference** — its
terminal block has no ground pin, and the ISO unit reaches SHIELD only through 1 MΩ ∥ 1 nF. If the
mechanism is common-mode drift accumulating on a floating isolated side, it would apply to the
Waveshare as well. Do not read §3 as evidence of long-run reliability.

## 5. Refactor regression check, AtomS3

Before/after on the same board, back to back, with only the firmware changing. Both runs yielded
to a GLM adapter driving the bus, so neither transmitted.

| | `main` | `Rs485Profile` refactor |
|---|---|---|
| chars / frames | 34,495 / 3,663 | 43,234 / 4,598 |
| `start rej` | 0 | 0 |
| `stop2` / chars | 0.041% | 0.035% |
| `framing errs` / chars | 0.293% | 0.294% |
| `crc errs` / frames | 0.055% | 0.043% |
| `C0 alias` / frames | 0.655% | 0.609% |

Every difference is inside noise: **the refactor is behaviour-neutral on the receive path.**

Two limits worth being explicit about. Because both runs were yielding, neither exercised the
refactor's *transmit*-side changes — the conditional echo-skip arming and the cancel-only guard
never ran, and the ISR direction release is a no-op on a board with no direction pin. And the
absolute rates come from a bus configuration later abandoned (two controllers plus an adapter
attached), so they are comparable with each other and with nothing else in this document.

## Not yet measured

- **The refactor's transmit path on the AtomS3.** §5 covers reception only. Exercising the echo
  skip and its guard on that board needs a master-mode before/after, which means flashing `main`
  onto it again.
- **Long-run behaviour under load, on any board other than the ISO unit.** This is the gap that
  matters most. §4's bus losses only appeared over hours of real listening; the longest run for
  §3's two boards is ~25 minutes with the system idle, which is not the same test. Until the
  Waveshare has been soaked under load it is unproven on the axis that disqualified the ISO unit —
  and being isolated with no ground reference, it shares the characteristic that is the leading
  suspect.
- **What on the ISO unit's transmit side is actually at fault.** §4 localises it to its own
  driving and rules out the receive path, but distinguishing the per-bit `DE` switching time from
  anything else needs a scope on the direction line against the A−B differential.
- **Double-ended termination**, i.e. one terminator at the controller and one at the last speaker.
- **A ground reference.** The board's terminal block has no ground pin; the internal header is the
  only access. Not needed for correct operation in this setup.

## Method

```bash
python3 tools/capture_run.py --label "<what is different about this run>" \
    --yaml espgensam-waveshare-local.yaml --device espgensam-waveshare.local
```

Raw logs are written to `captures/` (gitignored) with the run label in the filename.
