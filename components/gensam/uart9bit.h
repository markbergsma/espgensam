#pragma once

/// @file uart9bit.h
/// @brief 9-bit RS-485 transceiver driver using ESP32 RMT (Remote Control Peripheral).
///
/// ===================================================================================
/// ARCHITECTURE & DESIGN RATIONALE
/// ===================================================================================
/// The Genelec Smart Active Monitor (SAM / "GLM") proprietary bus operates over RS-485
/// at 288,000 baud using 9 data bits (8 data bits + 1 address/data indicator) and 2 stop
/// bits. A 9th bit value of 1 marks an address byte, while 0 marks data, CRC, or delimiter.
///
/// Standard ESP32 hardware UARTs cannot be used directly for this protocol:
/// 1. Lack of native 9-bit multidrop support: The ESP32 hardware UART does not provide a
///    native 9-bit data mode. Emulating the 9th bit using parity switching (Mark/Space parity)
///    requires reconfiguring the UART peripheral registers on the fly.
/// 2. Inter-byte gap & Auto-direction transceiver failure:
///    Dynamically reconfiguring UART registers between characters introduces a 10–50 µs
///    latency gap between consecutive bytes. Off-the-shelf RS-485 modules (such as the
///    M5Stack Atomic RS485 Base) rely on an automatic direction circuit driven by TX
///    transitions. When an inter-byte gap occurs, the direction circuit drops Driver Enable
///    (DE) prematurely, cutting off or corrupting subsequent bytes mid-frame.
///
/// To achieve 100% reliable, zero-modification operation with off-the-shelf hardware:
/// - Transmitter (RMT TX): Entire multi-byte frames (start bit, 8 data bits, 9th bit,
///   and 2 stop bits) are serialized into a single continuous pulse stream using the RMT
///   peripheral. The entire frame transmits with 0 ns inter-byte gap, keeping the
///   auto-direction transceiver circuit continuously asserted throughout the packet.
///   A Q16 fixed-point bit accumulator at 10 MHz resolution (100 ns/tick) provides exact,
///   zero-drift baud rate timing across frames of any length.
/// - Receiver (RMT RX): Captures raw bus edge transitions into ping-pong symbol buffers
///   with hardware glitch filtering and a 50 µs idle threshold. An IRAM-resident ISR
///   scans symbols in a single O(N) sweep, confirms the Start bit center, center-samples
///   data and 9th address bits, checks stop bits (with fallback to Stop Bit 2 center to
///   tolerate slow rise times on passive pull-up transceivers), and pushes decoded characters
///   into a FreeRTOS ring buffer.
/// - Half-Duplex Reception: Half-duplex transceivers loop back transmitted pulses into RX.
///   Incoming characters are decoded continuously without blocking delay in write(), avoiding
///   race conditions with fast monitor replies. Echo rejection and GLM address filtering are
///   performed deterministically at the protocol stream layer.
///
/// Echo suppression, and why it is conditional:
///   Whether any of a transmission reaches RX is a property of the board, not of the protocol.
///   A transceiver whose direction line is driven deliberately, asserted before the first bit,
///   is deaf for the whole frame and leaks nothing. An auto-direction module also ties /RE to DE
///   and so does not loop whole frames back either -- but its one-shot is triggered by our own
///   start bit, so it is necessarily late and the opening bits escape. That leak is short and
///   forms its own RX burst; it is what the skip below actually absorbs. See rs485.h topology
///   (a) for the measurements. The skip is armed only when Rs485Profile::tx_echoes_rx says so,
///   and a guard in the RX ISR may *cancel* an armed skip but may never create or resize one.
///
///   That asymmetry is what makes the arrangement safe, and it matters more than it looks. The
///   armed skip is a whole frame long, while the leak it is meant to absorb is a fragment. If
///   the leak's burst never materialises, the next burst is the monitor's reply -- and on the
///   auto-direction board that is the common case, 613 of 618 post-transmission bursts. Applying
///   a frame-length skip there would destroy the reply outright. The guard cancels it, because
///   that burst began after the transmitter stopped. A skip that is too long or wrongly applied
///   eats the start bit of a reply arriving 5-10 us after our last stop bit, which is the
///   failure mode AGENTS.md records as having been introduced and reverted twice. A skip that
///   fails to apply merely lets a fragment through, and BusArbiter::classify() discards it at
///   the frame layer.
///
///   The guard works by timestamp. write() records when the transmitter actually stopped (in the
///   TX-done ISR, not from task context, so queueing latency is excluded), and the RX ISR
///   reconstructs when its burst began by summing the symbol durations -- the callback fires
///   after the burst ends plus the idle threshold, so arrival is not otherwise knowable. A burst
///   that began after the transmitter stopped cannot contain our echo, so the skip is dropped and
///   @ref RxDecodeSnapshot::echo_skip_cancelled counts it. The discriminant is a whole frame time
///   (250-500 us at 288 kbaud) against single-digit-microsecond ISR latency, so the margin is two
///   orders of magnitude. The skip *magnitude* stays exactly the transmitted frame's tick count
///   and is never derived from a timestamp.
///
///   This relies on the TX-done ISR running before the RX-done ISR for the same exchange, which
///   it does by construction: RMT reports a burst complete only after the idle threshold (50 us)
///   has elapsed past its last edge, whereas TX-done fires at the final stop bit. Should that
///   ordering ever be violated, the guard compares against the *previous* transmission and
///   cancels, so our echo reaches the frame layer and BusArbiter::classify() discards it -- the
///   degraded outcome, not the destructive one. Every failure here is arranged to fall that way.
///
///   Rejected alternatives, so they are not re-proposed:
///   - Comparing the burst's span against the skip length ("too short to be ours"). Fails
///     whenever the reply is longer than the query, which is the normal case.
///   - Matching the burst's first decoded character against the first transmitted one. Cheap and
///     nearly exact, but it puts protocol knowledge inside the driver and collides whenever a
///     reply happens to open with the same byte.
/// ===================================================================================

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "driver/rmt_rx.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_types.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "soc/soc_caps.h"

#include "rs485.h"
#include "uart9bit_char.h"

namespace esphome {
namespace gensam {

// Uart9BitChar now lives in uart9bit_char.h so that frame.h can be compiled without ESP-IDF.

/// @brief Character-level decode outcome, as a plain value.
///
/// The three failure counters separate causes that a single "framing error" tally conflates,
/// which is what makes receive-path quality measurable:
/// - @ref start_rejects  A falling edge was found but the Start bit center did not sample LOW.
///                       Line noise, or an edge arriving while the receiver is still settling.
/// - @ref stopbit2_rescues  Stop Bit 1 sampled LOW but Stop Bit 2 sampled HIGH — the character
///                       was recovered only by the fallback. On a correctly matched baud rate
///                       this fires for genuinely slow pull-up rise times, so it is the leading
///                       indicator of transceiver quality and the metric to compare across a
///                       hardware change. Framing errors are the lagging indicator.
/// - @ref framing_errs   Neither stop position sampled HIGH; the character is lost outright.
///
/// @ref echo_skip_cancelled is not a fault count. It tallies how often the guard described at the
/// top of this file suppressed an armed echo skip because the burst began after the transmitter
/// stopped. On an auto-direction board it should sit at or near zero; a climbing count there means
/// echo bursts are going missing between transmission and reception, which is worth chasing. On a
/// board whose /RE is tied to DE the skip is never armed, so this stays at zero by construction.
///
/// This is the snapshot type handed to reporting code. Each counter is a naturally aligned
/// 32-bit word so an individual read cannot tear, and copying the whole set at once means a
/// log line's arithmetic and its printed columns describe the same instant, rather than
/// drifting apart as the ISR keeps counting between reads.
struct RxDecodeSnapshot {
  uint32_t chars{0};             ///< Characters successfully decoded.
  uint32_t addr_chars{0};        ///< Of those, characters with the 9th bit set (address bytes).
  uint32_t data_chars{0};        ///< Of those, characters with the 9th bit clear.
  uint32_t start_rejects{0};     ///< Falling edges rejected at the Start bit center check.
  uint32_t stopbit2_rescues{0};  ///< Characters salvaged only by sampling Stop Bit 2.
  uint32_t framing_errs{0};      ///< Characters lost: neither stop bit position sampled HIGH.
  uint32_t echo_skip_cancelled{0};  ///< Armed echo skips suppressed by the timestamp guard.
};

/// @brief Character-level decode tally.
///
/// Counters are written from the RX ISR and read from the main task, hence @c volatile.
/// Read them through @ref snapshot() rather than field by field.
struct RxDecodeStats {
  volatile uint32_t chars{0};
  volatile uint32_t addr_chars{0};
  volatile uint32_t data_chars{0};
  volatile uint32_t start_rejects{0};
  volatile uint32_t stopbit2_rescues{0};
  volatile uint32_t framing_errs{0};
  volatile uint32_t echo_skip_cancelled{0};

  RxDecodeSnapshot snapshot() const {
    RxDecodeSnapshot s;
    s.chars = chars;
    s.addr_chars = addr_chars;
    s.data_chars = data_chars;
    s.start_rejects = start_rejects;
    s.stopbit2_rescues = stopbit2_rescues;
    s.framing_errs = framing_errs;
    s.echo_skip_cancelled = echo_skip_cancelled;
    return s;
  }
};

/// @brief 9-bit RS-485 transceiver driver utilizing the ESP-IDF 5.x RMT peripheral.
class Uart9Bit {
 public:
  Uart9Bit() = default;

  /// @brief Destructor: cleans up RMT channels, encoders, and ring buffers.
  ~Uart9Bit();

  // Not copyable or movable (owns hardware resources).
  Uart9Bit(const Uart9Bit &) = delete;
  Uart9Bit &operator=(const Uart9Bit &) = delete;

  /// Initialize the RMT RX and RMT TX drivers.
  ///
  /// A profile whose Rs485Profile::tx_pin is negative brings up the receiver only, leaving the
  /// RMT TX channel uncreated; that is how listen-only mode guarantees bus silence.
  /// @param profile The board's RS-485 front end. Copied; the caller need not keep it alive.
  /// @param baud_rate Baud rate in bps (288000 for GLM RS485 bus).
  /// @param rx_buffer_size RX ring buffer capacity in Uart9BitChar units.
  void setup(const Rs485Profile &profile, uint32_t baud_rate = 288000,
             size_t rx_buffer_size = 512);

  /// Read up to @p max_chars 9-bit characters from the RX ring buffer.
  /// @param buf Output buffer for received characters.
  /// @param max_chars Maximum number of characters to read.
  /// @param timeout FreeRTOS tick count to wait for the first character.
  ///                Use 0 for non-blocking.
  /// @return Number of characters actually read.
  size_t read(Uart9BitChar *buf, size_t max_chars, TickType_t timeout = 0);

  /// Write 9-bit characters to the RS485 bus as a continuous, zero-gap RMT bitstream.
  /// @param chars Characters to transmit.
  /// @param len Number of characters.
  void write(const Uart9BitChar *chars, size_t len);

  /// Number of characters currently available in the RX ring buffer.
  size_t available() const;

  /// Whether the driver has been successfully initialized.
  bool is_initialized() const { return initialized_; }

  // --- Diagnostic counters ---------------------------------------------------

  /// Total number of 9-bit characters received.
  uint32_t rx_char_count() const { return stats_.chars; }

  /// Number of characters where the 9th bit was 1 (address bytes).
  uint32_t rx_addr_count() const { return stats_.addr_chars; }

  /// Number of characters where the 9th bit was 0 (data/payload/CRC/delimiter bytes).
  uint32_t rx_data_count() const { return stats_.data_chars; }

  /// Number of pulse burst events captured by RMT.
  uint32_t rx_burst_count() const { return rx_burst_count_; }

  /// Number of character framing errors (neither stop bit position sampled HIGH).
  uint32_t rx_framing_err_count() const { return stats_.framing_errs; }

  /// Number of characters salvaged only by falling back to the Stop Bit 2 center.
  uint32_t rx_stopbit2_count() const { return stats_.stopbit2_rescues; }

  /// Number of falling edges rejected because the Start bit center did not sample LOW.
  uint32_t rx_start_reject_count() const { return stats_.start_rejects; }

  /// Consistent snapshot of the full decode tally.
  RxDecodeSnapshot rx_stats() const { return stats_.snapshot(); }

  /// @brief Worst observed overshoot past a frame's own duration before the direction line was
  /// released, in microseconds. Zero on boards without a direction pin.
  ///
  /// This is an **upper bound** on release latency, not a measurement of it, and the distinction
  /// matters when comparing hardware. The anchor is taken in task context immediately before
  /// rmt_transmit(), so anything delaying the hardware from actually starting -- most often the
  /// WiFi or API task preempting us -- is charged here despite elapsing *before* the frame goes
  /// out, where no monitor is replying and it cannot cost a reply. Only the portion after the
  /// final stop bit is harmful, and separating the two needs a scope on the direction line.
  ///
  /// Read it together with @ref tx_overshoot_count *and* the reply-loss counters. The count on
  /// its own does not separate a harmless delay from a harmful one. A large overshoot on a
  /// sizeable fraction of transmissions, alongside @ref framing_errs and CRC counts that stay at
  /// zero, can only mean the delay falls ahead of the frame, where the bus is idle and no monitor
  /// is answering. So a rising count beside flat reply-loss counters is scheduling noise; a
  /// rising count beside rising reply loss is a real release stall. Neither number means much
  /// alone, and neither is comparable across builds that release the line differently.
  uint32_t tx_overshoot_max_us() const { return tx_overshoot_max_us_; }

  /// @brief Transmissions whose overshoot exceeded @c TX_OVERSHOOT_WARN_US.
  uint32_t tx_overshoot_count() const { return tx_overshoot_count_; }

 private:
  /// RMT RX event callback (called from ISR when a pulse burst completes).
  static bool rmt_rx_done_callback_(rmt_channel_handle_t rx_chan,
                                    const rmt_rx_done_event_data_t *edata,
                                    void *user_ctx);

  /// RMT TX event callback (called from ISR the moment a transmission completes).
  ///
  /// Releasing the direction line here rather than after rmt_tx_wait_all_done() is the whole
  /// point: see rs485.h section 3.
  static bool rmt_tx_done_callback_(rmt_channel_handle_t tx_chan,
                                    const rmt_tx_done_event_data_t *edata,
                                    void *user_ctx);

  /// Record the moment transmission ended and release the direction line if it is asserted.
  ///
  /// Called from the TX-done ISR, and from write()'s error paths where no ISR will fire. Safe to
  /// call redundantly. The timestamp is recorded unconditionally, including on boards with no
  /// direction pin, because the RX guard depends on it.
  void note_tx_complete_();

  /// Decodes RMT pulse symbols into 9-bit characters and pushes to ring buffer.
  /// @param symbols Pointer to received RMT symbol words.
  /// @param num_symbols Number of symbol words in the buffer.
  /// @param skip_ticks Number of hardware ticks to skip at the start of the burst (self-transmitted echo).
  void decode_and_push_symbols_(const rmt_symbol_word_t *symbols,
                                size_t num_symbols, uint32_t skip_ticks = 0);

  rmt_channel_handle_t rmt_rx_chan_{nullptr};
  rmt_channel_handle_t rmt_tx_chan_{nullptr};
  rmt_encoder_handle_t rmt_tx_encoder_{nullptr};
  RingbufHandle_t rx_ringbuf_{nullptr};
  Rs485Profile profile_{};
  bool initialized_{false};

  // Ping-pong symbol buffers for continuous RMT reception.
  static constexpr size_t RMT_SYM_BUF_SIZE = 512;
  rmt_symbol_word_t rx_symbols_a_[RMT_SYM_BUF_SIZE];
  rmt_symbol_word_t rx_symbols_b_[RMT_SYM_BUF_SIZE];
  rmt_symbol_word_t *active_rx_buf_{nullptr};
  rmt_receive_config_t receive_config_{};

  // Dedicated buffer for continuous RMT multi-byte frame transmission.
  static constexpr size_t RMT_TX_MAX_SYMBOLS = 256;
  rmt_symbol_word_t tx_symbols_[RMT_TX_MAX_SYMBOLS];

  uint32_t baud_rate_{288000};

  // --- TX / RX handover state -----------------------------------------------
  // Written by write() in task context and by the TX-done ISR; read by the RX ISR.
  //
  // The two timestamps are the low 32 bits of esp_timer_get_time(), not the full int64. A 32-bit
  // word cannot tear when an ISR on the other core reads it mid-update, whereas a 64-bit one can,
  // and truncation costs nothing: every interval compared here is sub-millisecond, so the
  // wrap-safe signed difference below stays correct across the ~71 minute rollover.
  volatile uint32_t tx_echo_ticks_{0};   ///< Armed echo skip for the next burst, in RMT ticks.
  volatile uint32_t tx_frame_ticks_{0};  ///< Duration of the frame just transmitted, in ticks.
  volatile uint32_t tx_start_us_{0};     ///< When rmt_transmit() was issued.
  volatile uint32_t tx_end_us_{0};       ///< When transmission completed.
  volatile bool de_asserted_{false};     ///< Whether the direction line is currently driving.

  /// Overshoot beyond which a transmission is counted as anomalous, in microseconds.
  /// 100 us is ~2.4 character times at 288 kbaud: far longer than any legitimate ISR latency,
  /// far shorter than the millisecond-scale stalls a preempted task produces.
  static constexpr uint32_t TX_OVERSHOOT_WARN_US = 100;

  // Diagnostic counters (updated from ISR, read from main task).
  RxDecodeStats stats_{};
  volatile uint32_t rx_burst_count_{0};
  volatile uint32_t tx_overshoot_max_us_{0};
  volatile uint32_t tx_overshoot_count_{0};
};

}  // namespace gensam
}  // namespace esphome
