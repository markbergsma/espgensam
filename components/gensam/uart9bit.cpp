/// @file uart9bit.cpp
/// @brief Implementation of 9-bit RS-485 transceiver driver using RMT RX + continuous RMT TX.
/// See uart9bit.h for architectural design rationale and complete API documentation.

#include "uart9bit.h"

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "soc/soc_caps.h"

static const char *const TAG = "gensam.uart9bit";

namespace esphome {
namespace gensam {

// ---------------------------------------------------------------------------
// Helper: Cursor for traversing RMT pulse symbols without heap allocation
// ---------------------------------------------------------------------------

namespace {

/// @brief Lightweight cursor for traversing RMT pulse symbols sequentially without dynamic memory allocations.
struct RmtCursor {
  const rmt_symbol_word_t *symbols{nullptr};
  size_t num_symbols{0};
  size_t sym_idx{0};
  int half{0};
  uint32_t curr_time{0};  // Start time in ticks of the current half
  uint32_t dur{0};        // Duration in ticks of the current half
  uint8_t lvl{1};         // Signal level (0 or 1) of the current half

  /// @brief Initialize the cursor at the beginning of an RMT pulse symbol array.
  /// @param syms Pointer to raw RMT symbol words.
  /// @param n Total number of symbol words.
  void init(const rmt_symbol_word_t *syms, size_t n) {
    symbols = syms;
    num_symbols = n;
    sym_idx = 0;
    half = 0;
    curr_time = 0;
    load_current();
  }

  /// @brief Advance through empty or zero-duration symbol halves to load the next active level and duration.
  void load_current() {
    while (sym_idx < num_symbols) {
      if (half == 0) {
        dur = symbols[sym_idx].duration0;
        lvl = symbols[sym_idx].level0;
        if (dur > 0) return;
        half = 1;
      }
      if (half == 1) {
        dur = symbols[sym_idx].duration1;
        lvl = symbols[sym_idx].level1;
        if (dur > 0) return;
        half = 0;
        sym_idx++;
      }
    }
    dur = 0;
    lvl = 1;  // Default idle HIGH at end of stream
  }

  /// @brief Step the time cursor forward past the current symbol half.
  void advance() {
    curr_time += dur;
    if (half == 0) {
      half = 1;
    } else {
      half = 0;
      sym_idx++;
    }
    load_current();
  }

  /// @brief Advance the cursor monotonically to absolute timestamp @p t and sample the signal level.
  /// @param t Target timestamp in RMT ticks (100 ns units).
  /// @return Signal level (0 = LOW, 1 = HIGH).
  uint8_t sample_at(uint32_t t) {
    while (dur > 0 && (curr_time + dur) <= t) {
      advance();
    }
    if (dur > 0 && t >= curr_time && t < (curr_time + dur)) {
      return lvl;
    }
    return 1;  // Default idle HIGH
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

/// @brief Destructor: disables and tears down RMT RX/TX channels, TX copy encoder, and ring buffer.
Uart9Bit::~Uart9Bit() {
  if (rmt_rx_chan_) {
    rmt_disable(rmt_rx_chan_);
    rmt_del_channel(rmt_rx_chan_);
    rmt_rx_chan_ = nullptr;
  }
  if (rmt_tx_chan_) {
    rmt_disable(rmt_tx_chan_);
    rmt_del_channel(rmt_tx_chan_);
    rmt_tx_chan_ = nullptr;
  }
  if (rmt_tx_encoder_) {
    rmt_del_encoder(rmt_tx_encoder_);
    rmt_tx_encoder_ = nullptr;
  }
  if (rx_ringbuf_) {
    vRingbufferDelete(rx_ringbuf_);
    rx_ringbuf_ = nullptr;
  }
  initialized_ = false;
}

/// @brief Initialize hardware peripherals for 9-bit RS-485 operation.
///
/// Configures RX GPIO pull-up, optional DE hardware direction control, RMT TX continuous
/// transmitter with copy encoder, FreeRTOS byte ring buffer, and RMT RX channel with
/// 500 ns glitch filter, 50 µs idle burst detector, and ping-pong symbol buffers.
/// @param port Port number (reserved for interface compatibility).
/// @param tx_pin GPIO number for TX.
/// @param rx_pin GPIO number for RX.
/// @param de_pin Optional GPIO number for RS-485 DE/RE direction control (-1 if unused).
/// @param baud_rate Baud rate in bps (default 281,250 for Genelec GLM).
/// @param rx_buffer_size RX ring buffer capacity in Uart9BitChar units.
void Uart9Bit::setup(int port, int tx_pin, int rx_pin, int de_pin,
                     uint32_t baud_rate, size_t rx_buffer_size) {
  baud_rate_ = baud_rate;
  de_pin_ = de_pin;

  // --- 0. Configure RX GPIO Pull-up ----------------------------------------
  gpio_config_t rx_gpio_cfg = {};
  rx_gpio_cfg.pin_bit_mask = (1ULL << rx_pin);
  rx_gpio_cfg.mode = GPIO_MODE_INPUT;
  rx_gpio_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
  rx_gpio_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  rx_gpio_cfg.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&rx_gpio_cfg);

  // --- 0b. Optional DE/RE Direction Pin Configuration -----------------------
  if (de_pin_ >= 0) {
    gpio_config_t de_cfg = {};
    de_cfg.pin_bit_mask = (1ULL << de_pin_);
    de_cfg.mode = GPIO_MODE_OUTPUT;
    de_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    de_cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
    de_cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&de_cfg);
    gpio_set_level(static_cast<gpio_num_t>(de_pin_), 0);  // Default RX (listen)
  }

  // --- 1. RMT TX Channel Configuration (Zero-Gap Continuous Stream) ---------
  if (tx_pin >= 0) {
    // Pre-configure TX GPIO HIGH before RMT binds to prevent auto-direction transceivers from asserting DE
    gpio_config_t tx_gpio_cfg = {};
    tx_gpio_cfg.pin_bit_mask = (1ULL << tx_pin);
    tx_gpio_cfg.mode = GPIO_MODE_OUTPUT;
    tx_gpio_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    tx_gpio_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    tx_gpio_cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&tx_gpio_cfg);
    gpio_set_level(static_cast<gpio_num_t>(tx_pin), 1);

    rmt_tx_channel_config_t tx_channel_cfg = {};
    tx_channel_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    tx_channel_cfg.resolution_hz = 10000000;  // 10 MHz resolution = 100 ns/tick
#if defined(SOC_RMT_MEM_WORDS_PER_CHANNEL)
    tx_channel_cfg.mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL;
#else
    tx_channel_cfg.mem_block_symbols = 64;
#endif
    tx_channel_cfg.gpio_num = static_cast<gpio_num_t>(tx_pin);
    tx_channel_cfg.trans_queue_depth = 4;
    tx_channel_cfg.flags.invert_out = 0;
    tx_channel_cfg.flags.with_dma = 0;

    esp_err_t err = rmt_new_tx_channel(&tx_channel_cfg, &rmt_tx_chan_);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "rmt_new_tx_channel failed: %s (err=0x%x)", esp_err_to_name(err), err);
      return;
    }

    rmt_copy_encoder_config_t copy_encoder_cfg = {};
    err = rmt_new_copy_encoder(&copy_encoder_cfg, &rmt_tx_encoder_);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "rmt_new_copy_encoder failed: %s", esp_err_to_name(err));
      rmt_del_channel(rmt_tx_chan_);
      rmt_tx_chan_ = nullptr;
      return;
    }

    err = rmt_enable(rmt_tx_chan_);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "rmt_enable (TX) failed: %s", esp_err_to_name(err));
      rmt_del_channel(rmt_tx_chan_);
      rmt_tx_chan_ = nullptr;
      return;
    }

    // Immediately latch RMT TX hardware output to idle HIGH (mark level)
    rmt_symbol_word_t init_sym = {};
    init_sym.level0 = 1;
    init_sym.duration0 = 10;
    init_sym.level1 = 1;
    init_sym.duration1 = 10;
    rmt_transmit_config_t init_tx_cfg = {};
    init_tx_cfg.loop_count = 0;
    init_tx_cfg.flags.eot_level = 1;
    rmt_transmit(rmt_tx_chan_, rmt_tx_encoder_, &init_sym, sizeof(init_sym), &init_tx_cfg);
    rmt_tx_wait_all_done(rmt_tx_chan_, 50);
  }

  // --- 2. RX Ring Buffer ----------------------------------------------------
  rx_ringbuf_ = xRingbufferCreate(rx_buffer_size * sizeof(Uart9BitChar),
                                  RINGBUF_TYPE_BYTEBUF);
  if (!rx_ringbuf_) {
    ESP_LOGE(TAG, "Failed to create RX ring buffer (%u chars)",
             (unsigned)rx_buffer_size);
    return;
  }

  // --- 3. RMT RX Channel Configuration --------------------------------------
  rmt_rx_channel_config_t rx_channel_cfg = {};
  rx_channel_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
  rx_channel_cfg.resolution_hz = 10000000;  // 10 MHz resolution = 100 ns/tick
#if defined(SOC_RMT_MEM_WORDS_PER_CHANNEL)
  rx_channel_cfg.mem_block_symbols = SOC_RMT_MEM_WORDS_PER_CHANNEL;
#else
  rx_channel_cfg.mem_block_symbols = 64;
#endif
  rx_channel_cfg.gpio_num = static_cast<gpio_num_t>(rx_pin);
  rx_channel_cfg.flags.invert_in = 0;
  rx_channel_cfg.flags.with_dma = 0;

  esp_err_t err = rmt_new_rx_channel(&rx_channel_cfg, &rmt_rx_chan_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "rmt_new_rx_channel failed: %s (err=0x%x)", esp_err_to_name(err), err);
    vRingbufferDelete(rx_ringbuf_);
    rx_ringbuf_ = nullptr;
    return;
  }

  rmt_rx_event_callbacks_t cbs = {
      .on_recv_done = rmt_rx_done_callback_,
  };
  err = rmt_rx_register_event_callbacks(rmt_rx_chan_, &cbs, this);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "rmt_rx_register_event_callbacks failed: %s",
             esp_err_to_name(err));
    rmt_del_channel(rmt_rx_chan_);
    rmt_rx_chan_ = nullptr;
    vRingbufferDelete(rx_ringbuf_);
    rx_ringbuf_ = nullptr;
    return;
  }

  err = rmt_enable(rmt_rx_chan_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "rmt_enable (RX) failed: %s", esp_err_to_name(err));
    rmt_del_channel(rmt_rx_chan_);
    rmt_rx_chan_ = nullptr;
    vRingbufferDelete(rx_ringbuf_);
    rx_ringbuf_ = nullptr;
    return;
  }

  // 500ns glitch filter, 50µs idle threshold (triggers end-of-frame callback)
  receive_config_.signal_range_min_ns = 500;
  receive_config_.signal_range_max_ns = 50000;

  active_rx_buf_ = rx_symbols_a_;
  err = rmt_receive(rmt_rx_chan_, active_rx_buf_, sizeof(rx_symbols_a_),
                    &receive_config_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "initial rmt_receive failed: %s", esp_err_to_name(err));
    rmt_disable(rmt_rx_chan_);
    rmt_del_channel(rmt_rx_chan_);
    rmt_rx_chan_ = nullptr;
    vRingbufferDelete(rx_ringbuf_);
    rx_ringbuf_ = nullptr;
    return;
  }

  initialized_ = true;
  ESP_LOGI(TAG,
           "Uart9Bit initialized: TX=RMT (GPIO%d), RX=RMT (GPIO%d), DE=GPIO%d, "
           "baud=%lu, tick=100ns, buf=%u chars",
           tx_pin, rx_pin, de_pin_, (unsigned long)baud_rate_,
           (unsigned)rx_buffer_size);
}

// ---------------------------------------------------------------------------
// ISR-side: RMT RX Done Callback
// ---------------------------------------------------------------------------

/// @brief RMT RX ISR callback triggered when a pulse burst completes (after 50 µs idle).
///
/// Immediately re-arms the RMT channel with the alternate ping-pong symbol buffer to ensure
/// zero dropped bursts, then decodes the completed burst into 9-bit characters.
/// @param rx_chan Handle to the RMT RX channel.
/// @param edata Event data containing received symbol pointer and count.
/// @param user_ctx Pointer to the Uart9Bit instance.
/// @return Always false (no high-priority task wake requested).
bool IRAM_ATTR Uart9Bit::rmt_rx_done_callback_(
    rmt_channel_handle_t rx_chan, const rmt_rx_done_event_data_t *edata,
    void *user_ctx) {
  auto *self = static_cast<Uart9Bit *>(user_ctx);

  rmt_symbol_word_t *received_buf = edata->received_symbols;
  size_t num_symbols = edata->num_symbols;

  // Immediately re-arm RMT with alternate ping-pong buffer
  rmt_symbol_word_t *next_buf = (received_buf == self->rx_symbols_a_)
                                    ? self->rx_symbols_b_
                                    : self->rx_symbols_a_;
  self->active_rx_buf_ = next_buf;
  rmt_receive(rx_chan, next_buf, sizeof(self->rx_symbols_a_),
              &self->receive_config_);

  self->rx_burst_count_++;

  uint32_t skip_ticks = self->tx_echo_ticks_;
  self->tx_echo_ticks_ = 0;

  // Decode symbols directly in ISR and push decoded 9-bit chars to ring buffer
  if (num_symbols > 0) {
    self->decode_and_push_symbols_(received_buf, num_symbols, skip_ticks);
  }

  return false;
}

// ---------------------------------------------------------------------------
// IRAM symbol decoder: Extracts 9-bit characters with fixed-point math
// ---------------------------------------------------------------------------

/// @brief IRAM-resident pulse decoder converting raw RMT symbols into decoded 9-bit characters.
///
/// Performs an O(N) single-pass sweep over pulse durations using Q16 fixed-point math:
/// 1. Fast-forwards past self-transmitted loopback echo pulses (if skip_ticks > 0).
/// 2. Finds falling edges and confirms the Start bit center level (LOW).
/// 3. Center-samples 8 data bits (LSB-first).
/// 4. Center-samples the 9th bit (1 = address, 0 = data).
/// 5. Verifies Stop bit level (HIGH), tracking framing errors if violated.
/// 6. Pushes valid characters into the FreeRTOS ring buffer via xRingbufferSendFromISR().
/// @param symbols Pointer to RMT symbol words.
/// @param num_symbols Number of symbol words in the buffer.
/// @param skip_ticks Number of hardware ticks to skip at the start of the burst (self-transmitted echo).
void IRAM_ATTR Uart9Bit::decode_and_push_symbols_(
    const rmt_symbol_word_t *symbols, size_t num_symbols, uint32_t skip_ticks) {
  RmtCursor cursor;
  cursor.init(symbols, num_symbols);

  // Fast-forward cursor past self-transmitted loopback echo
  if (skip_ticks > 0) {
    while (cursor.dur > 0 && (cursor.curr_time + cursor.dur) <= skip_ticks) {
      cursor.advance();
    }
  }

  // Q16 fixed-point arithmetic for 10 MHz tick rate (100 ns/tick)
  // At 281,250 baud: 10,000,000 / 281,250 = 35.5555... ticks/bit
  // In Q16: (10,000,000 * 65536) / 281,250 = 2330168
  const uint32_t bit_ticks_q16 =
      static_cast<uint32_t>((10000000ULL * 65536ULL) / baud_rate_);
  const uint32_t half_bit_ticks_q16 = bit_ticks_q16 / 2;

  while (cursor.dur > 0) {
    // 1. Find Start Bit: search for falling edge (HIGH -> LOW)
    while (cursor.dur > 0 && cursor.lvl != 0) {
      cursor.advance();
    }
    if (cursor.dur == 0) {
      break;  // End of burst
    }

    uint32_t t_start = cursor.curr_time;

    // Verify Start bit: sample at t_start + 0.5 bit
    uint32_t sample_t = t_start + (half_bit_ticks_q16 >> 16);
    if (cursor.sample_at(sample_t) != 0) {
      cursor.advance();
      continue;
    }

    // 2. Sample 8 Data Bits (D0–D7)
    uint8_t data = 0;
    uint32_t t_bit_center_q16 =
        (static_cast<uint64_t>(t_start) << 16) + bit_ticks_q16 + half_bit_ticks_q16;

    for (int b = 0; b < 8; b++) {
      uint8_t bit_val = cursor.sample_at(t_bit_center_q16 >> 16);
      data |= (bit_val << b);
      t_bit_center_q16 += bit_ticks_q16;
    }

    // 3. Sample 9th Bit (Address / Data Marker)
    uint8_t ninth_bit = cursor.sample_at(t_bit_center_q16 >> 16);
    t_bit_center_q16 += bit_ticks_q16;

    // 4. Sample Stop Bit (must be HIGH = 1)
    uint8_t stop_bit = cursor.sample_at(t_bit_center_q16 >> 16);
    if (stop_bit != 1) {
      // Check Stop Bit 2 center in case slow passive pull-up delayed Stop Bit 1 rise
      stop_bit = cursor.sample_at((t_bit_center_q16 + bit_ticks_q16) >> 16);
    }
    if (stop_bit != 1) {
      rx_framing_err_count_++;
      // Resynchronize: advance past any continuing LOW pulse to ensure the next
      // Start bit hunt begins from a confirmed HIGH (idle) state
      while (cursor.dur > 0 && cursor.lvl == 0) {
        cursor.advance();
      }
      continue;
    }

    // Advance cursor past the stop bit
    uint32_t t_end = (t_bit_center_q16 + bit_ticks_q16) >> 16;
    while (cursor.dur > 0 && (cursor.curr_time + cursor.dur) <= t_end) {
      cursor.advance();
    }

    // Push valid 9-bit character to FreeRTOS ring buffer
    Uart9BitChar c;
    c.data = data;
    c.ninth_bit = ninth_bit;

    xRingbufferSendFromISR(rx_ringbuf_, &c, sizeof(c), nullptr);

    rx_char_count_++;
    if (ninth_bit) {
      rx_addr_count_++;
    } else {
      rx_data_count_++;
    }
  }
}

// ---------------------------------------------------------------------------
// RX — main-task side
// ---------------------------------------------------------------------------

/// @brief Read decoded 9-bit characters from the RX ring buffer.
///
/// Uses xRingbufferReceiveUpTo() to retrieve up to @p max_chars characters without dropping
/// excess characters when returning items to FreeRTOS.
/// @param buf Destination buffer for received characters.
/// @param max_chars Maximum number of characters to read.
/// @param timeout FreeRTOS tick timeout to wait for the first item (0 for non-blocking).
/// @return Actual number of characters copied to @p buf.
size_t Uart9Bit::read(Uart9BitChar *buf, size_t max_chars,
                      TickType_t timeout) {
  if (!initialized_ || !buf || max_chars == 0) {
    return 0;
  }

  size_t chars_read = 0;
  while (chars_read < max_chars) {
    size_t item_size = 0;
    size_t max_bytes_needed = (max_chars - chars_read) * sizeof(Uart9BitChar);
    void *item = xRingbufferReceiveUpTo(rx_ringbuf_, &item_size,
                                        (chars_read == 0) ? timeout : 0,
                                        max_bytes_needed);
    if (!item) {
      break;
    }

    size_t count = item_size / sizeof(Uart9BitChar);
    memcpy(&buf[chars_read], item, count * sizeof(Uart9BitChar));
    chars_read += count;

    vRingbufferReturnItem(rx_ringbuf_, item);
  }

  return chars_read;
}

/// @brief Query the number of decoded 9-bit characters currently available in the RX ring buffer.
/// @return Available character count.
size_t Uart9Bit::available() const {
  if (!initialized_ || !rx_ringbuf_) {
    return 0;
  }
  UBaseType_t waiting = 0;
  vRingbufferGetInfo(rx_ringbuf_, nullptr, nullptr, nullptr, nullptr, &waiting);
  return waiting / sizeof(Uart9BitChar);
}

// ---------------------------------------------------------------------------
// TX — Zero-Gap Continuous RMT Bitstream Transmitter
// ---------------------------------------------------------------------------

/// @brief Transmit a multi-byte 9-bit frame as an uninterrupted continuous RMT bitstream.
///
/// Converts each 9-bit character into Start bit, 8 data bits, 9th address bit, and 2 stop
/// bits, emitting the entire frame with 0 ns inter-byte gap to maintain auto-direction
/// transceiver engagement. Deasserts hardware DE immediately upon transmission completion.
/// @param chars Array of 9-bit characters to transmit.
/// @param len Number of characters in @p chars.
void Uart9Bit::write(const Uart9BitChar *chars, size_t len) {
  if (!initialized_ || !rmt_tx_chan_ || len == 0) {
    return;
  }

  // Assert Direction Enable (TX Mode) if hardware DE pin configured
  if (de_pin_ >= 0) {
    gpio_set_level(static_cast<gpio_num_t>(de_pin_), 1);
    esp_rom_delay_us(5);
  }

  // Q16 fixed-point math for zero-drift bit timing at 10 MHz resolution (100 ns/tick)
  // At 281,250 baud: 10,000,000 / 281,250 = 35.5555... ticks/bit
  const uint32_t bit_ticks_q16 =
      static_cast<uint32_t>((10000000ULL * 65536ULL) / baud_rate_);

  uint32_t bit_cursor_q16 = 0;
  size_t sym_idx = 0;
  int half = 0;

  auto emit_bit = [&](uint8_t lvl) {
    if (sym_idx >= RMT_TX_MAX_SYMBOLS) return;

    uint32_t next_cursor_q16 = bit_cursor_q16 + bit_ticks_q16;
    uint16_t dur = static_cast<uint16_t>((next_cursor_q16 >> 16) - (bit_cursor_q16 >> 16));
    bit_cursor_q16 = next_cursor_q16;

    if (dur == 0) dur = 1;

    if (half == 0) {
      tx_symbols_[sym_idx].level0 = lvl;
      tx_symbols_[sym_idx].duration0 = dur;
      half = 1;
    } else {
      tx_symbols_[sym_idx].level1 = lvl;
      tx_symbols_[sym_idx].duration1 = dur;
      half = 0;
      sym_idx++;
    }
  };

  // Render entire multi-byte frame back-to-back with 0 nanosecond inter-byte gap
  for (size_t i = 0; i < len; i++) {
    uint8_t data = chars[i].data;
    uint8_t ninth_bit = chars[i].ninth_bit;

    // 1. Start Bit (LOW = 0)
    emit_bit(0);

    // 2. 8 Data Bits (LSB first)
    for (int b = 0; b < 8; b++) {
      emit_bit((data >> b) & 1);
    }

    // 3. 9th Bit (1 = Address, 0 = Data)
    emit_bit(ninth_bit ? 1 : 0);

    // 4. 2 Stop Bits (HIGH = 1)
    emit_bit(1);
    emit_bit(1);
  }

  // If frame ends on an odd half-symbol, pad the second half with idle HIGH
  if (half == 1) {
    uint32_t next_cursor_q16 = bit_cursor_q16 + bit_ticks_q16;
    uint16_t dur = static_cast<uint16_t>((next_cursor_q16 >> 16) - (bit_cursor_q16 >> 16));
    if (dur == 0) dur = 36;
    tx_symbols_[sym_idx].level1 = 1;
    tx_symbols_[sym_idx].duration1 = dur;
    sym_idx++;
  }

  // Blast out full frame continuously via hardware RMT.
  // Set echo skip window to total frame duration plus 2 bit times (~7 µs) post-TX settling margin.
  tx_echo_ticks_ = (bit_cursor_q16 >> 16) + (2 * (bit_ticks_q16 >> 16));
  rmt_transmit_config_t tx_config = {};
  tx_config.loop_count = 0;
  tx_config.flags.eot_level = 1;  // End-of-transmission level: 1 (Idle HIGH / Mark)
  esp_err_t err = rmt_transmit(rmt_tx_chan_, rmt_tx_encoder_, tx_symbols_,
                               sym_idx * sizeof(rmt_symbol_word_t), &tx_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "rmt_transmit failed: %s (err=0x%x, syms=%u)",
             esp_err_to_name(err), err, (unsigned)sym_idx);
  } else {
    rmt_tx_wait_all_done(rmt_tx_chan_, 100);

    // Return to Receive (RX Mode) immediately after transmission completes
    if (de_pin_ >= 0) {
      esp_rom_delay_us(5);
      gpio_set_level(static_cast<gpio_num_t>(de_pin_), 0);
    }
  }
}

}  // namespace gensam
}  // namespace esphome
