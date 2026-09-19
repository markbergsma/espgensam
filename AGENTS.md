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
  The primary aim is to make this work with unmodified, off-the-shelf development boards and modules (such as the M5Stack AtomS3 Lite + Atomic RS485 Base and LilyGO T-CAN485). Additional options requiring e.g. external pull-up resistors, custom soldering, or hardware modifications can be added only if needed.
- **Continuous Zero-Gap RMT Transmission**:
  The Genelec GLM RS-485 bus operates at 288,000 baud with 9 data bits and 2 stop bits. Because  many off-the-shelf transceivers (such as the MAX13487 or Atomic RS-485 auto-direction circuit) sense TX transitions to assert Driver Enable (DE), all multi-byte frames must be transmitted via RMT as a single, uninterrupted pulse train with **0 ns inter-byte gap**.
- **Half-Duplex Echo Suppression**:
  Half-duplex RS-485 transceivers echo transmitted bytes back onto the RX line. The receiver driver must wait for the RMT RX idle threshold (50 µs) to expire post-transmission (settling window ~80 µs), then completely flush the RX ring buffer before incoming monitor replies arrive.

---

## 4. Git, Build & Verification Workflows

- **Git operations**
  You NEVER make any git commits wihout explicit approval from the user. While you can make suggestions for commit messages, they should be reviewed or edited by the user. Add an "Assisted-by <model name>" header if you contributed to the change, e.g. "Assisted-by: Gemini 3.8".

- **ESPHome Compilation**:
  Verify any C++ or YAML modifications by compiling with ESPHome:
  ```bash
  esphome compile espgensam.yaml
  ```
  Ensure builds complete with zero compiler errors or unhandled warnings.
- **Credential Safety**:
  Never commit private network credentials or keys. Always keep sensitive parameters in `secrets.yaml` (which is excluded by `.gitignore`) and provide sanitized templates in `secrets.yaml.example`.

