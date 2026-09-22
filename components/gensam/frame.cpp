/// @file frame.cpp
/// @brief Frame serialization, byte stuffing, and FrameParser implementation.
/// See frame.h for protocol framing specifications and API documentation.

#include "frame.h"
#include "crc.h"

#include <cstdio>
#include <cstring>

namespace esphome {
namespace gensam {

void escape_bytes(const uint8_t *in, size_t len, std::vector<uint8_t> &out) {
  out.clear();
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    uint8_t b = in[i];
    if (b == FRAME_DELIMITER || b == FRAME_ESCAPE) {
      out.push_back(FRAME_ESCAPE);
      out.push_back(b ^ ESCAPE_XOR);
    } else {
      out.push_back(b);
    }
  }
}

namespace {

template <typename T, typename Getter>
void unescape_impl(const T *in, size_t len, std::vector<uint8_t> &out, Getter get) {
  out.clear();
  out.reserve(len);
  for (size_t i = 0; i < len; i++) {
    uint8_t b = get(in[i]);
    if (b == FRAME_ESCAPE && i + 1 < len) {
      uint8_t next = get(in[i + 1]);
      if (next == (FRAME_DELIMITER ^ ESCAPE_XOR) || next == (FRAME_ESCAPE ^ ESCAPE_XOR)) {
        out.push_back(next ^ ESCAPE_XOR);
        i++;
        continue;
      }
    }
    out.push_back(b);
  }
}

}  // namespace

void unescape_bytes(const uint8_t *in, size_t len, std::vector<uint8_t> &out) {
  unescape_impl(in, len, out, [](uint8_t b) { return b; });
}

void unescape_bytes(const Uart9BitChar *in, size_t len, std::vector<uint8_t> &out) {
  unescape_impl(in, len, out, [](const Uart9BitChar &c) { return c.data; });
}

std::vector<Uart9BitChar> Frame::to_9bit() const {
  // 1. Calculate CRC-16/GSM over [Address, Command, Payload]
  uint8_t check_stack[MAX_FRAME_LENGTH];
  const size_t check_len = 2 + payload.size();
  std::vector<uint8_t> check_heap;
  uint8_t *check_ptr = check_stack;

  if (check_len > sizeof(check_stack)) {
    check_heap.resize(check_len);
    check_ptr = check_heap.data();
  }

  check_ptr[0] = address;
  check_ptr[1] = command;
  if (!payload.empty()) {
    std::memcpy(&check_ptr[2], payload.data(), payload.size());
  }

  uint16_t crc = calculate_crc(check_ptr, check_len);
  uint8_t crc_hi = static_cast<uint8_t>((crc >> 8) & 0xFF);
  uint8_t crc_lo = static_cast<uint8_t>(crc & 0xFF);

  // 2. Build 9-bit wire character sequence in a single pass
  std::vector<Uart9BitChar> out;
  // Worst-case capacity: 1 addr + (1 cmd + payload + 2 crc) * 2 + 1 delim
  out.reserve(1 + (3 + payload.size()) * 2 + 1);

  // Address word (9th bit = 1, unescaped)
  out.push_back(Uart9BitChar{address, NINTH_BIT_FLAG});

  // Lambda to escape body/CRC bytes directly into the output vector
  auto push_escaped = [&out](uint8_t b) {
    if (b == FRAME_DELIMITER || b == FRAME_ESCAPE) {
      out.push_back(Uart9BitChar{FRAME_ESCAPE, 0});
      out.push_back(Uart9BitChar{static_cast<uint8_t>(b ^ ESCAPE_XOR), 0});
    } else {
      out.push_back(Uart9BitChar{b, 0});
    }
  };

  // Command byte (9th bit = 0)
  push_escaped(command);

  // Payload bytes (9th bit = 0)
  for (uint8_t b : payload) {
    push_escaped(b);
  }

  // CRC bytes (9th bit = 0, MSB first)
  push_escaped(crc_hi);
  push_escaped(crc_lo);

  // Delimiter word (9th bit = 0)
  out.push_back(Uart9BitChar{FRAME_DELIMITER, 0});
  return out;
}

std::string Frame::to_string() const {
  char buf[128];
  int written = snprintf(buf, sizeof(buf), "addr=0x%02X cmd=0x%02X payload=[", address, command);
  std::string s(buf, written);
  for (size_t i = 0; i < payload.size(); i++) {
    char hex[8];
    snprintf(hex, sizeof(hex), "%s%02X", i > 0 ? " " : "", payload[i]);
    s += hex;
  }
  s += "]";
  if (!crc_valid) {
    s += " [crc?]";
  }
  return s;
}

void FrameParser::feed(const Uart9BitChar *chars, size_t count) {
  for (size_t i = 0; i < count; i++) {
    feed(chars[i]);
  }
}

void FrameParser::feed(const Uart9BitChar &c) {
  if (c.ninth_bit != 0) {
    uint8_t addr = c.data;

    // Hardware Turnaround Workaround:
    // On half-duplex RS-485 modules with auto-direction circuitry (such as the M5Stack Atomic
    // RS-485 Base), fast-replying monitors (e.g. 7350A subwoofer or 8330A replying within ~5-10 µs)
    // begin transmitting before the transceiver's receiver circuit has completely settled.
    //
    // The UART wire encoding for HOST_ADDRESS (0x01, LSB-first) is:
    //   [Start=0] [D0=1] [D1=0] [D2=0] [D3=0] [D4=0] [D5=0] [D6=0] [D7=0] [9th=1] [Stop1=1] [Stop2=1]
    // If transceiver release delays detection of the initial falling edge, the receiver misses
    // the Start bit (0). Since D0 is HIGH (1) like the idle line, the first falling edge seen by the
    // RMT digitizer is the D0 -> D1 transition. The decoder locks onto that edge as the "Start bit",
    // mathematically shifting the sampled bits by 1 bit time:
    //   Decoded bits: D1..D7 + 9th + Stop1 = [0, 0, 0, 0, 0, 0, 1, 1] = 0xC0 (with 9th bit = 1).
    // The rest of the frame (command, payload, CRC, delimiter) arrives completely uncorrupted.
    //
    // Fast-replying monitor responses destined for HOST_ADDRESS (0x01) can experience 1-bit
    // transceiver turnaround lag on passive-pull RS-485 modules, decoding as 0xC0'.
    // We map 0xC0' to HOST_ADDRESS. Full integrity is strictly protected by the 16-bit CRC check
    // in process_candidate_(); any corrupted noise frame will fail CRC and be dropped.
    //
    // The lost start edge happens at the *bus* turnaround -- the previous master's driver
    // releasing, the line coasting to its idle bias, the monitor's driver asserting. It is
    // therefore a property of whoever released the bus, not of our own transmission: captures
    // glm_startup_capture.log and glm_v5_no_wakeup_capture.log contain no TX frames at all and
    // still show the alias, and in glm_source_select_capture.log 47 of 50 occurrences fall after
    // the hub had yielded and stopped transmitting entirely.
    //
    // The dominant factor is bus termination, because it sets how fast the line coasts back to a
    // valid idle mark once a driver releases. Measured on the Waveshare ESP32-S3-RS485-CAN as bus
    // master, one variable, 4,000+ frames each (docs/rs485-transceiver-comparison.md):
    //
    //   no terminator:  2,850 aliases / 4,000 frames = 71.25%   (RC ~4.7 us, longer than a bit)
    //   one 120 ohm:       37 aliases / 4,257 frames =  0.87%   (RC ~60 ns)
    //
    // Unterminated, the line has not settled when the monitor's start bit arrives 5-10 us later,
    // so the edge is missed. Both runs delivered every frame correctly -- but the unterminated one
    // only because this workaround repaired 71% of them.
    //
    // So the workaround stays, and c0_alias_count() is as much a measure of bus wiring as of the
    // transceiver. Compare hardware only with termination pinned, and never pool samples across a
    // configuration change.
    //
    // So the workaround stays: it is nearly idle while we drive the bus, and still earning its
    // keep at ~0.8% whenever we yield to GLM, which is whenever a GLM adapter is present.
    // Removing it would require never yielding. Track c0_alias_count() to compare hardware, and
    // read it separately for master and yielded phases -- a single pooled figure mostly measures
    // how much of the sample was spent yielding.
    if (addr == 0xC0) {
      addr = HOST_ADDRESS;
      c0_alias_count_++;
    }

    // Only accept bytes that represent valid GLM destination addresses.
    // Transceiver loopback echo with distorted rise times often samples false 9th bits (e.g. BF', DF', AD', 7F').
    if (!is_valid_glm_address(addr)) {
      return;
    }

    // Address character marks start of a new frame
    if (state_ == State::ACCUMULATING) {
      // Previous frame was incomplete / aborted on wire
      invalid_count_++;
    }
    buffer_.clear();
    buffer_.push_back({addr, c.ninth_bit});
    state_ = State::ACCUMULATING;
  } else {
    // Data / CRC / Delimiter character
    if (state_ != State::ACCUMULATING) {
      // Ignore leading noise characters before an address byte
      return;
    }

    if (c.data == FRAME_DELIMITER) {
      // Delimiter reached; process frame candidate
      process_candidate_();
      buffer_.clear();
      state_ = State::IDLE;
    } else {
      buffer_.push_back(c);
      if (buffer_.size() > MAX_FRAME_LENGTH) {
        // Frame exceeded maximum allowed length; discard
        invalid_count_++;
        buffer_.clear();
        state_ = State::IDLE;
      }
    }
  }
}

void FrameParser::process_candidate_() {
  if (buffer_.empty()) {
    return;
  }

  // buffer_[0] is address with 9th bit = 1
  uint8_t address = buffer_[0].data;
  if (!is_valid_glm_address(address)) {
    invalid_count_++;
    return;
  }

  // Unescape buffer_[1..N-1] (escaped body and CRC bytes)
  std::vector<uint8_t> unescaped;
  unescape_bytes(&buffer_[1], buffer_.size() - 1, unescaped);

  // Must contain at least command (1 byte) + CRC (2 bytes)
  if (unescaped.size() < 3) {
    invalid_count_++;
    return;
  }

  uint8_t command = unescaped[0];
  size_t pld_len = unescaped.size() - 3;
  std::vector<uint8_t> payload;
  if (pld_len > 0) {
    payload.assign(unescaped.begin() + 1, unescaped.begin() + 1 + pld_len);
  }

  uint16_t wire_crc = (static_cast<uint16_t>(unescaped[unescaped.size() - 2]) << 8) |
                      static_cast<uint16_t>(unescaped[unescaped.size() - 1]);

  // Check CRC over [Address, Command, Payload]
  uint8_t check_stack[MAX_FRAME_LENGTH];
  const size_t check_len = 2 + payload.size();
  std::vector<uint8_t> check_heap;
  uint8_t *check_ptr = check_stack;

  if (check_len > sizeof(check_stack)) {
    check_heap.resize(check_len);
    check_ptr = check_heap.data();
  }

  check_ptr[0] = address;
  check_ptr[1] = command;
  if (!payload.empty()) {
    std::memcpy(&check_ptr[2], payload.data(), payload.size());
  }

  bool crc_ok = verify_crc(check_ptr, check_len, wire_crc);
  if (crc_ok) {
    valid_count_++;
  } else {
    crc_mismatch_count_++;
  }

  frame_queue_.emplace_back(address, command, std::move(payload), crc_ok);
}

bool FrameParser::pop_frame(Frame &out) {
  if (frame_queue_.empty()) {
    return false;
  }
  out = std::move(frame_queue_.front());
  frame_queue_.pop_front();
  return true;
}

void FrameParser::clear() {
  buffer_.clear();
  frame_queue_.clear();
  state_ = State::IDLE;
}

}  // namespace gensam
}  // namespace esphome

