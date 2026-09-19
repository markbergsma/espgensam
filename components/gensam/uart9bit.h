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

namespace esphome {
namespace gensam {

/// @brief A single 9-bit character received from or to be transmitted on the bus.
struct Uart9BitChar {
  uint8_t data;       ///< 8-bit data value (D0–D7).
  uint8_t ninth_bit;  ///< 9th bit: 1 = address byte, 0 = data byte.

  Uart9BitChar() : data(0), ninth_bit(0) {}
  Uart9BitChar(uint8_t d, uint8_t n) : data(d), ninth_bit(n) {}
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
  /// @param port Reserved for compatibility.
  /// @param tx_pin GPIO number for TX.
  /// @param rx_pin GPIO number for RX.
  /// @param de_pin Optional GPIO number for RS485 DE/RE direction control (-1 if unused).
  /// @param baud_rate Baud rate in bps (288000 for GLM RS485 bus).
  /// @param rx_buffer_size RX ring buffer capacity in Uart9BitChar units.
  void setup(int port, int tx_pin, int rx_pin, int de_pin = -1,
             uint32_t baud_rate = 288000, size_t rx_buffer_size = 512);

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
  uint32_t rx_char_count() const { return rx_char_count_; }

  /// Number of characters where the 9th bit was 1 (address bytes).
  uint32_t rx_addr_count() const { return rx_addr_count_; }

  /// Number of characters where the 9th bit was 0 (data/payload/CRC/delimiter bytes).
  uint32_t rx_data_count() const { return rx_data_count_; }

  /// Number of pulse burst events captured by RMT.
  uint32_t rx_burst_count() const { return rx_burst_count_; }

  /// Number of character framing errors (e.g. invalid stop bit).
  uint32_t rx_framing_err_count() const { return rx_framing_err_count_; }

 private:
  /// RMT RX event callback (called from ISR when a pulse burst completes).
  static bool rmt_rx_done_callback_(rmt_channel_handle_t rx_chan,
                                    const rmt_rx_done_event_data_t *edata,
                                    void *user_ctx);

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
  int de_pin_{-1};
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
  volatile uint32_t tx_echo_ticks_{0};

  // Diagnostic counters (updated from ISR, read from main task).
  volatile uint32_t rx_char_count_{0};
  volatile uint32_t rx_addr_count_{0};
  volatile uint32_t rx_data_count_{0};
  volatile uint32_t rx_burst_count_{0};
  volatile uint32_t rx_framing_err_count_{0};
};

}  // namespace gensam
}  // namespace esphome
