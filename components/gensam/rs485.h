#pragma once

/// @file rs485.h
/// @brief Static description of a board's RS-485 front end, and the pin primitives that drive it.
///
/// ===================================================================================
/// ARCHITECTURE & DESIGN RATIONALE
/// ===================================================================================
/// Everything that varies between supported boards, as far as the protocol core is concerned, is
/// a value in @ref Rs485Profile. The board's YAML package fills it in; nothing below this header
/// knows which board it is running on. Adding a board is a package plus, at most, a new field.
///
/// 1. Three transceiver topologies, and why the difference is not cosmetic:
///
///    a) Auto-direction (M5Stack Atomic RS-485 Base). No direction pin at all: the module senses
///       TX transitions and asserts Driver Enable itself, and its /RE is tied to DE just as in
///       (c), so it does NOT loop whole frames back into RX. What leaks is the *opening* of a
///       transmission: the one-shot is triggered by our own start bit, so the first bit or two
///       goes out while the receiver is still enabled, and only then does /RE deassert.
///
///       Measured on captures/glm_standby_capture.log, AtomS3 as bus master: 613 of 618 RX
///       bursts immediately following a transmission begin with the monitor's reply address,
///       not with our data. Fragments of our own frame (e.g. `FF' FE'`, the head of a RACE
///       ping) appear at the *tail* of a preceding burst, which is where the leak lands.
///
///       The visible consequence is fragmentation rather than echo: that board runs at 2.8 RX
///       bursts per decoded frame, where an explicit-DE board runs at exactly 1.0. This is also
///       the topology uart9bit.h's zero-gap TX requirement exists for -- any inter-byte gap lets
///       the one-shot expire and drops DE mid-frame.
///
///    b) Auto-direction behind discrete enables (LilyGO T-CAN485, MAX13487). Direction is still
///       automatic, but the module needs a 5 V booster (@ref Rs485Profile::power_pin), a
///       transceiver enable (@ref Rs485Profile::se_pin) and a receiver enable
///       (@ref Rs485Profile::re_pin) asserted once at boot and then left alone. Echoes like (a).
///
///    c) Explicit direction (Waveshare ESP32-S3-RS485-CAN, SP3485EN). One GPIO
///       (@ref Rs485Profile::de_pin) drives the direction line per frame. On this board U7's /RE
///       (pin 2) and DE (pin 3) share a single net, so asserting it turns the driver on *and the
///       receiver off*. Two consequences follow, and both are load-bearing:
///
/// 2. A tied /RE means there is no echo.
///    The receiver is deaf for the duration of the transmission, so nothing loops back. That
///    breaks the assumption behind Uart9Bit's echo skip: the skip is armed at the end of write()
///    and consumed by the *next* RX burst, which on such a board is the monitor's genuine reply,
///    arriving 5-10 us later. Fast-forwarding past it destroys it. Hence
///    @ref Rs485Profile::tx_echoes_rx, and hence the guard documented in uart9bit.h.
///
/// 3. A tied /RE means DE release latency is a receive blackout.
///    On (a) and (b) a late release would only risk bus contention. Here every microsecond
///    between the last stop bit and the direction line going low is a microsecond the receiver is
///    switched off, landing precisely on the reply's start bit -- the same lost-start-edge
///    signature that shows up as the 0xC0 address alias (see frame.cpp). That is why release
///    happens in the RMT TX-done ISR and not from task context.
///
///    This is measured, not assumed. Releasing from task context after rmt_tx_wait_all_done()
///    instead, with everything else held constant, costs 28 CRC-failed frames and 133 framing
///    errors per 4,000 frames, against zero of each when released from the ISR. Reception also
///    visibly fragments: burst count exceeds frame count as replies arrive split across bursts,
///    where the ISR path yields exactly one burst per frame. See
///    docs/rs485-transceiver-comparison.md section 2.
///
/// 4. Why a plain struct and not a transceiver class.
///    The per-frame direction control has to execute inside the RMT TX-done ISR, on the object
///    that owns the TX channel, so it cannot move out of Uart9Bit without adding a pointer hop to
///    the exact latency path point 3 is about. The one-shot enables in (b) have to complete, with
///    their settling delay, before RMT binds the pins, so they cannot move out of the hub's
///    setup() ordering. A class would own neither behaviour and would only add indirection
///    between the two places that genuinely need to differ: the YAML package and this struct.
/// ===================================================================================

#include <cstdint>

#include "driver/gpio.h"
#include "hal/gpio_ll.h"

namespace esphome {
namespace gensam {

/// @brief Everything the protocol core needs to know about a board's RS-485 front end.
///
/// A pin of -1 means "this board does not have one". Polarity flags mirror ESPHome's `inverted:`
/// pin option; all three boards known today are active-high, but the option is accepted in YAML
/// and would otherwise be silently ignored.
struct Rs485Profile {
  int tx_pin{-1};       ///< RMT pulse generator output. -1 in listen-only mode.
  bool tx_inverted{false};
  int rx_pin{-1};       ///< RMT pulse digitizer input.
  bool rx_inverted{false};

  /// Dynamic Driver Enable, asserted per frame. On boards that tie /RE to DE this also gates RX.
  int de_pin{-1};
  bool de_active_high{true};

  /// Receiver Enable, asserted once at boot and held for the life of the program.
  int re_pin{-1};
  bool re_active_high{true};

  /// DC-DC booster enable for modules with their own 5 V rail.
  int power_pin{-1};
  bool power_active_high{true};

  /// Transceiver enable / shutdown, asserted once at boot.
  int se_pin{-1};
  bool se_active_high{true};

  /// @brief Whether any of our own transmission can reach the receive path.
  ///
  /// Not "does the whole frame loop back" -- on the auto-direction modules supported here it does
  /// not (see topology (a) above). What it means is that *something* of ours can appear on RX
  /// around a transmission, because the direction circuit is triggered by our own start bit and
  /// so is necessarily late. That leak forms its own short RX burst, which is what the echo skip
  /// in uart9bit.h is absorbed by.
  ///
  /// False when the direction line is driven deliberately and asserted before the first bit, so
  /// nothing of ours ever reaches the receiver. Codegen derives this from whether @ref de_pin is
  /// configured, which is correct for every board supported today; YAML can override it.
  bool tx_echoes_rx{true};

  /// @brief Settling time between asserting the direction line and starting to transmit.
  ///
  /// Generous: an SP3485's t_ZH is tens of nanoseconds, and a digital isolator in the path adds
  /// propagation delay and channel-to-channel skew of the same order. The delay precedes our own
  /// transmission on an idle bus, so it costs nothing that matters -- unlike the release side,
  /// which races the reply.
  uint32_t de_assert_settle_us{5};

  /// @brief Whether a per-frame direction line is configured.
  bool has_direction_control() const { return de_pin >= 0; }
};

/// @brief Configure a GPIO as an output and drive it to an initial state.
/// @param pin GPIO number; negative is a no-op.
/// @param active_high False when the pin is wired active-low.
/// @param asserted Logical state to drive, before polarity is applied.
/// @param pull_up Whether to enable the internal pull-up (for open-drain-ish external circuits).
inline void rs485_configure_output(int pin, bool active_high, bool asserted, bool pull_up = false) {
  if (pin < 0) {
    return;
  }
  gpio_config_t cfg = {};
  cfg.pin_bit_mask = (1ULL << pin);
  cfg.mode = GPIO_MODE_OUTPUT;
  cfg.pull_up_en = pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
  cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  cfg.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&cfg);
  gpio_set_level(static_cast<gpio_num_t>(pin), (asserted == active_high) ? 1 : 0);
}

/// @brief Drive an already-configured output pin, safe to call from an ISR.
///
/// Uses gpio_ll_set_level() rather than gpio_set_level(): the latter lives in flash unless
/// CONFIG_GPIO_CTRL_FUNC_IN_IRAM is set, which this project does not set, whereas the LL function
/// is a header-only static inline that compiles into the caller. This is what ESP-IDF's own
/// IRAM-safe drivers do. Task-context callers should prefer @ref rs485_configure_output or plain
/// gpio_set_level(); this exists for the RMT TX-done path.
///
/// always_inline rather than IRAM_ATTR, deliberately: marking a header inline function IRAM_ATTR
/// emits an out-of-line comdat copy in its own .iram1 section, and the Xtensa linker then rejects
/// it with "dangerous relocation: l32r: literal placed after use" because the literal pool lands
/// past the instruction referencing it. Forcing the body into the caller sidesteps that entirely,
/// and the caller (Uart9Bit::release_direction_) is what carries IRAM_ATTR.
/// @param pin GPIO number; negative is a no-op.
/// @param active_high False when the pin is wired active-low.
/// @param asserted Logical state to drive, before polarity is applied.
__attribute__((always_inline)) inline void rs485_write_pin(int pin, bool active_high,
                                                           bool asserted) {
  if (pin < 0) {
    return;
  }
  gpio_ll_set_level(&GPIO, static_cast<gpio_num_t>(pin), (asserted == active_high) ? 1 : 0);
}

}  // namespace gensam
}  // namespace esphome
