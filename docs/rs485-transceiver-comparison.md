# RS-485 bus measurements: Waveshare ESP32-S3-RS485-CAN

Controlled measurements on a live three-speaker GLM bus, 2026-09-22.

Every run here was taken with `tools/capture_run.py`, which marks the counters *after* discovery
completes, accumulates a fixed number of frames, and aborts if the device reboots or yields
mid-sample. One variable changes per run and the configuration is recorded in the label.

For why the code is shaped the way it is, see [`rs485.h`](../components/gensam/rs485.h) and
[`uart9bit.h`](../components/gensam/uart9bit.h).

## Setup

| | |
|---|---|
| Board | Waveshare ESP32-S3-RS485-CAN — SP3485EN, `/RE` tied to `DE` on GPIO21, galvanically isolated |
| Pins | TX GPIO17, RX GPIO18, direction GPIO21 |
| Bus | Three speakers (7350A + 2× 8330A), 288,000 baud |
| On the bus | **Waveshare only.** No GLM adapter, no AtomS3, no oscilloscope. |
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

## Not yet measured

- **Whether the `Rs485Profile` refactor is behaviour-neutral on the M5Stack AtomS3.** Needs a
  before/after pair on that board under a controlled, single-terminator bus.
- **The AtomS3 against the Waveshare**, both as bus master with one terminator. This is the
  comparison that motivated the work and it has not been made under controlled conditions.
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
