# RS-485 bus measurements

Controlled measurements on a live three-speaker GLM bus, 2026-09-22.

Every run here was taken with `tools/capture_run.py`, which marks the counters *after* discovery
completes, accumulates a fixed number of frames, and aborts if the device reboots or yields
mid-sample. One variable changes per run and the configuration is recorded in the label.

For why the code is shaped the way it is, see [`rs485.h`](../components/gensam/rs485.h) and
[`uart9bit.h`](../components/gensam/uart9bit.h).

## Setup

Sections 1 and 2 are the Waveshare; section 3 compares it against the M5Stack AtomS3 Lite +
Atomic RS485 Base and states that run's configuration separately.

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

## Not yet measured

- **Whether the `Rs485Profile` refactor is behaviour-neutral on the AtomS3.** Needs a before/after
  pair on that board, terminated. §3 measures the board, not the refactor.
- **Double-ended termination**, i.e. one terminator at the controller and one at the last speaker.
- **A ground reference.** The board's terminal block has no ground pin; the internal header is the
  only access. Not needed for correct operation in this setup.

## Method

```bash
python3 tools/capture_run.py --label "<what is different about this run>" \
    --yaml espgensam-waveshare-local.yaml --device espgensam-waveshare.local
```

Rules that make the numbers comparable, learned the hard way:

- **One variable per run**, named in the label.
- **Mark after discovery.** Errors cluster during RACE discovery and then stop; including them
  makes a steady-state rate meaningless.
- **Nothing else on the bus.** An oscilloscope probe is a variable — it adds capacitance and, on
  an isolated board, an earth reference.
- **Never compare across a reboot or a reflash** unless that is the variable.
- Counters are cumulative since boot, so every figure here is a delta.

Raw logs are written to `captures/` (gitignored) with the run label in the filename.
