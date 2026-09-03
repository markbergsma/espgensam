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
    // Address character marks start of a new frame
    if (state_ == State::ACCUMULATING) {
      // Previous frame was incomplete / aborted on wire
      invalid_count_++;
    }
    buffer_.clear();
    buffer_.push_back(c);
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
  if (!crc_ok) {
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

