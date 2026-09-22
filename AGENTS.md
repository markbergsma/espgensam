# Agent Guidelines & Repository Rules

This document defines architectural standards, hardware constraints, and coding guidelines for AI coding assistants working in the `espgensam` repository.

---

## 1. Code Documentation & Commenting Standards

### C++ Header Files (`.h`) — Single Source of Truth
- **Architectural Rationale**: Place comprehensive explanations of the hardware architecture, protocol requirements, physical layer timing, and design rationale at the top of the header file.
- **API Documentation**: Document all classes, structs, public/protected methods, and fields in the header using standard Doxygen tags (`@brief`, `@param`, `@return`).
- Explain *why* a particular design was chosen (e.g. peripheral constraints, transceiver characteristics), not just *what* the code does.

### C++ Implementation Files (`.cpp`) — Implementation Details Only
- **Header Reference**: At the top of the file, include a concise note referencing the corresponding `.h` header file for architectural design rationale and public API documentation.
- **No Redundant Doxygen Blocks**: Do **not** duplicate the `@brief`, `@param`, or `@return` docstrings already declared in the header file.
- **Local Scope Documentation**: Provide clean docstrings for internal file-local helpers (e.g. anonymous namespace structs, static helper functions).
- **Phase Markers**: Use clean section banners or brief inline comments to mark execution phases (e.g. `// 1. Find Start Bit`, `// --- TX bitstream ---`).

---

## 2. Architectural Principles & Separation of Concerns
### Three-Tier Layering
The codebase enforces a strict three-tier architecture to prevent coupling protocol timing with user-facing entities or physical peripherals:
1. **Protocol Core (`hub`, `arbiter`, `registry`, `race`, `snoop`, `uart9bit`, `frame`, `commands`)**:
   - Owns RS-485 wire protocol, 9-bit bitstream timings, transceiver direction control, state machines, and monitor tracking.
   - **Zero UI / Entity Awareness**: The core protocol layer must NEVER import, instantiate, or reference Home Assistant entities, light states, color math, or physical UI components.
   - **State Exposure**: Exposes internal transitions exclusively via public getters and observer callbacks (`add_state_callback()`, `add_bus_status_callback()`).
2. **Home Assistant Integration Layer (`<platform>.h`)**:
   - Partitioned strictly by ESPHome entity type: [`text_sensor.h`](file:///Users/mark/git/espgensam/components/gensam/text_sensor.h), [`sensor.h`](file:///Users/mark/git/espgensam/components/gensam/sensor.h), [`switch.h`](file:///Users/mark/git/espgensam/components/gensam/switch.h), [`number.h`](file:///Users/mark/git/espgensam/components/gensam/number.h), [`select.h`](file:///Users/mark/git/espgensam/components/gensam/select.h), [`button.h`](file:///Users/mark/git/espgensam/components/gensam/button.h).
   - **Single Responsibility**: Each entity class bridges between Home Assistant and the hub (subscribing to hub callbacks or dispatching HA actions).
   - **No Peripheral Control**: Entities do not drive physical LEDs, buzzers, or output pins directly.

---

## 3. Hardware Constraints & Principles

- **Off-The-Shelf (COTS) Preference**:
  The primary aim is to make this work with unmodified, off-the-shelf development boards and modules (such as the M5Stack AtomS3 Lite + Atomic RS485 Base, the Waveshare ESP32-S3-RS485-CAN, and LilyGO T-CAN485). Additional options requiring e.g. external pull-up resistors, custom soldering, or hardware modifications can be added only if needed.
- **Continuous Zero-Gap RMT Transmission**:
  The Genelec GLM RS-485 bus operates at 288,000 baud with 9 data bits and 2 stop bits. Because  many off-the-shelf transceivers (such as the MAX13487 or Atomic RS-485 auto-direction circuit) sense TX transitions to assert Driver Enable (DE), all multi-byte frames must be transmitted via RMT as a single, uninterrupted pulse train with **0 ns inter-byte gap**.
- **Half-Duplex Echo Suppression**:
  Some half-duplex RS-485 front ends let part of a transmission reach the RX line. Note this is *not* whole-frame loopback on any board supported here — the auto-direction modules tie `/RE` to `DE` as well, but their one-shot is triggered by our own start bit and so is necessarily late, letting the opening bits escape. That leak is discarded by fast-forwarding the RMT symbol cursor past **exactly** the transmitted frame's own duration (`tx_echo_ticks_`, the TX bit accumulator's final value), plus a `FrameParser::clear()` after each transmission. **Never add a blind settling delay or a blanket RX ring-buffer flush after TX**: monitors answer in ~5-10 µs, so any post-TX blind window eats the start bit of the reply. An earlier implementation waited 80 µs and flushed the ring buffer, and a later one padded the skip by just 2 bit times (~7 µs); both corrupted fast replies and were removed. **The skip must stay exact.**

  *Whether the skip is armed at all* is a board property, not a protocol invariant: a deliberately driven direction line is asserted before the first bit, so nothing of ours reaches the receiver, and skipping there would fast-forward past a genuine reply instead. `Rs485Profile::tx_echoes_rx` decides, and a timestamp guard in the RX ISR may **cancel** an armed skip but may never create or resize one. See [`rs485.h`](file:///Users/mark/git/espgensam/components/gensam/rs485.h) and [`uart9bit.h`](file:///Users/mark/git/espgensam/components/gensam/uart9bit.h). The rule above governs the magnitude and is unchanged.
- **Direction Line Release**:
  Where a board drives its own direction line, it is released from the RMT TX-done ISR, never from task context after `rmt_tx_wait_all_done()`. On a transceiver with `/RE` tied to `DE` the release latency is a receive blackout landing on the reply's start bit, so a preempted task loses the reply outright. `rmt_tx_wait_all_done()` stays in `write()`, but for *ordering* only — it must not be repurposed as direction-control timing.

---

## 3a. The board seam contract

Hardware variation is confined to two places. Do not add a third.

```
packages/board_*.yaml                  seam 1: the board — pins and peripherals
components/gensam/rs485.h              seam 2: Rs485Profile — transceiver behaviour
packages/gensam.yaml                   ── board-independent: monitors, entities, tunables
espgensam.yaml                         ── device identity, network, substitutions;
                                          one `board:` line selects the hardware
```

- **A board package declares peripherals and pins only.** No monitors, no volume bounds, no group presets, no Home Assistant entity that does not name a pin on that board. Adding a board must not require editing `packages/gensam.yaml`.
- **`packages/gensam.yaml` must never name a GPIO.**
- **Entity vs. binding**: the `rediscover_button:` *entity* is board-independent and lives in the shared package; the physical GPIO `binary_sensor` that triggers it belongs to the board. Same split for `bus_status:` (shared) and `status_led:` (board — it names a board-specific light id).
- **`bus_status:` is declared explicitly** in the shared package rather than left to the `status_led` validator to create, because a board with no RGB LED has no `status_led`, and `espgensam_dial` depends on that sensor existing.
- **Everything the protocol core needs to know about a board's front end is a value in `Rs485Profile`.** If a new board needs behaviour that is not expressible there, add a field — not a conditional, a subclass, or a board enum.

---

## 4. Git, Build & Verification Workflows

- **Git operations**
  You NEVER make any git commits wihout explicit approval from the user. While you can make suggestions for commit messages, they should be reviewed or edited by the user. Add an "Assisted-by <model name>" header if you contributed to the change, e.g. "Assisted-by: Gemini 3.8".

- **ESPHome Compilation**:
  Verify any C++ or YAML modifications by compiling with ESPHome:
  ```bash
  esphome compile espgensam.yaml
  ```
  There is one device config; the board is selected by the `board:` line in its `packages:` block. **Every supported board must build**, so swap that line to each `packages/board_*.yaml` in turn and repeat. Ensure builds complete with zero compiler errors or unhandled warnings. Schema changes also need `esphome config espgensam.yaml` per board, since the hub schema's pin validators need `CORE.target_platform` and cannot be exercised standalone.
- **Host Unit Tests**:
  Protocol parsers that are pure functions over a byte buffer are tested on the host, against
  verbatim payloads from `captures/`. No framework and no build system — compile the test
  together with the translation unit under test:
  ```bash
  c++ -std=c++17 -Wall -o /tmp/test_parse_telemetry \
      tests/test_parse_telemetry.cpp components/gensam/monitor.cpp && /tmp/test_parse_telemetry
  ```
  A new test must be shown to fail against the unfixed code before it is trusted.
- **Credential Safety**:
  Never commit private network credentials or keys. Always keep sensitive parameters in `secrets.yaml` (which is excluded by `.gitignore`) and provide sanitized templates in `secrets.yaml.example`.

