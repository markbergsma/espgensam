#pragma once

/// @file frame.h
/// @brief Packet framing, byte-stuffing, and stream parsing for Genelec SAM RS-485 bus.
///
/// ===================================================================================
/// PROTOCOL FRAMING & BYTE-STUFFING SPECIFICATION
/// ===================================================================================
/// The Genelec SAM ("GLM") protocol layers asynchronous multi-byte packets over a 9-bit
/// multidrop RS-485 physical layer.
///
/// 1. Frame Wire Structure:
///    ┌──────────────┬──────────────────┬──────────────┬──────────────┬──────────────┐
///    │ Address Byte │   Command Byte   │ Payload (0-N)│  CRC-16 GSM  │  Delimiter   │
///    │ (9th bit = 1)│   (9th bit = 0)  │ (9th bit = 0)│ (9th bit = 0)│ (9th bit = 0)│
///    │  [unescaped] │    [escaped]     │  [escaped]   │  [escaped]   │     0x7E     │
///    └──────────────┴──────────────────┴──────────────┴──────────────┴──────────────┘
///     ▲                                                              ▲
///     └───────────── CRC-16 Computed Over Raw Bytes ─────────────────┘
///
/// 2. 9th-Bit Address Flag & Escaping Rules:
///    - Address Byte: The first byte of every frame is transmitted with the 9th bit
///      set to 1. Because this unique 9th-bit tag identifies the address unambiguously,
///      the address byte is NEVER escaped, even if its numerical value is 0x7E or 0x7D.
///    - Body & CRC Bytes: Transmitted with the 9th bit cleared to 0. Any byte within the
///      command, payload, or 16-bit CRC matching the frame delimiter (0x7E) or escape byte
///      (0x7D) must be escaped using byte-stuffing:
///        0x7E -> [0x7D, 0x5E]  (0x7E XOR 0x20)
///        0x7D -> [0x7D, 0x5D]  (0x7D XOR 0x20)
///    - End-of-Frame Delimiter: A literal 0x7E with 9th bit = 0 terminates every frame.
///
/// 3. Checksum Verification:
///    CRC-16/GSM (polynomial 0x1021, init 0x0000, xorout 0xFFFF) is calculated over
///    [Address, Command, Payload] before transmission escaping, and verified after reception
///    unescaping.
///
/// 4. Stream Parser (FrameParser):
///    - Discards leading noise characters (9th bit = 0 while IDLE).
///    - Immediately resynchronizes on any valid 9th-bit address byte.
///    - In host_only mode, accommodates 1-bit RS-485 transceiver turnaround lag (where HOST_ADDRESS
///      0x01' is sampled as 0xC0' by fast-replying monitors) under strict 16-bit CRC validation.
///    - Accumulates body bytes up to MAX_FRAME_LENGTH (256) safety bound.
///    - Unescapes body and CRC on delimiter (0x7E) and verifies CRC checksum.
/// ===================================================================================

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "const.h"
#include "uart9bit.h"

namespace esphome {
namespace gensam {

/// @brief Escape body/CRC bytes (replaces 0x7E with [0x7D, 0x5E] and 0x7D with [0x7D, 0x5D]).
/// @param in Input raw byte buffer.
/// @param len Number of bytes in @p in.
/// @param[out] out Output vector containing escaped bytes.
void escape_bytes(const uint8_t *in, size_t len, std::vector<uint8_t> &out);

/// @brief Reverse byte-stuffing in received body/CRC bytes (from raw byte array).
/// @param in Input escaped byte buffer.
/// @param len Number of bytes in @p in.
/// @param[out] out Output vector containing unescaped raw bytes.
void unescape_bytes(const uint8_t *in, size_t len, std::vector<uint8_t> &out);

/// @brief Reverse byte-stuffing in received body/CRC bytes directly from a 9-bit character stream.
/// @param in Input array of Uart9BitChar.
/// @param len Number of characters in @p in.
/// @param[out] out Output vector containing unescaped raw bytes.
void unescape_bytes(const Uart9BitChar *in, size_t len, std::vector<uint8_t> &out);

/// @brief Check whether an address byte matches a valid Genelec GLM protocol destination.
///
/// Valid network addresses comprise:
/// - Host controller: 0x01 (HOST_ADDRESS)
/// - Unicast monitors: 0x02 to 0x20 (MONITOR_START_ADDR up to 32 devices)
/// - Multicast group: 0xF0 (MULTICAST_ADDRESS)
/// - Broadcast group: 0xFF (BROADCAST_ADDRESS)
/// @param addr The 8-bit address candidate to validate.
/// @return True if @p addr is a valid GLM network address.
inline bool is_valid_glm_address(uint8_t addr, bool host_only = false) {
  if (host_only) {
    return addr == HOST_ADDRESS;
  }
  return addr == HOST_ADDRESS ||
         (addr >= MONITOR_START_ADDR && addr <= 0x20) ||
         addr == MULTICAST_ADDRESS ||
         addr == BROADCAST_ADDRESS;
}

/// @brief A single decoded 9-bit GLM bus frame.
struct Frame {
  uint8_t address{0};            ///< Destination or source address byte (from 9th-bit word).
  uint8_t command{0};            ///< Protocol command opcode.
  std::vector<uint8_t> payload{};///< Unescaped command payload bytes.
  bool crc_valid{true};          ///< Whether CRC-16/GSM verification passed.

  Frame() = default;
  Frame(uint8_t addr, uint8_t cmd, std::vector<uint8_t> pld = {}, bool valid = true)
      : address(addr), command(cmd), payload(std::move(pld)), crc_valid(valid) {}

  /// @brief Check if this frame is an ACK reply (CMD_ACK = 0x01).
  bool is_ack() const { return command == CMD_ACK; }

  /// @brief Check if destination is the broadcast address (0xFF).
  bool is_broadcast() const { return address == BROADCAST_ADDRESS; }

  /// @brief Check if destination is the multicast address (0xF0).
  bool is_multicast() const { return address == MULTICAST_ADDRESS; }

  /// @brief Check if destination is the host controller address (0x01).
  bool is_host() const { return address == HOST_ADDRESS; }

  /// @brief Convert this frame to an escaped 9-bit character wire representation.
  ///
  /// Serializes the frame as:
  /// 1. Address byte (9th bit = 1, unescaped)
  /// 2. Escaped [Command + Payload + CRC-16/GSM] (9th bit = 0)
  /// 3. Frame delimiter 0x7E (9th bit = 0)
  /// @return Vector of Uart9BitChar ready for Uart9Bit::write().
  std::vector<Uart9BitChar> to_9bit() const;

  /// @brief Returns a concise human-readable debug string representation.
  std::string to_string() const;
};

/// @brief Incremental stream parser that reconstructs Frame objects from raw 9-bit characters.
class FrameParser {
 public:
  FrameParser() = default;

  /// @brief Feed a batch of 9-bit characters from the RX ring buffer.
  /// @param chars Pointer to array of received 9-bit characters.
  /// @param count Number of characters in @p chars.
  void feed(const Uart9BitChar *chars, size_t count);

  /// @brief Feed a single 9-bit character into the parser state machine.
  /// @param c The 9-bit character.
  void feed(const Uart9BitChar &c);

  /// @brief Check whether one or more completed frames are ready in the queue.
  /// @return True if completed frames are waiting.
  bool has_frames() const { return !frame_queue_.empty(); }

  /// @brief Pop the next completed frame from the queue.
  /// @param[out] out Destination frame object populated via move semantics.
  /// @return True if a frame was successfully popped, false if queue was empty.
  bool pop_frame(Frame &out);

  /// @brief Total count of malformed or oversized frame candidates encountered.
  uint32_t invalid_count() const { return invalid_count_; }

  /// @brief Total count of frames that failed CRC-16/GSM checksum verification.
  uint32_t crc_mismatch_count() const { return crc_mismatch_count_; }

  /// @brief Reset internal state machine, discarding any partial frame and queued frames.
  void clear();

  /// @brief Set whether parser only accepts frames addressed to HOST_ADDRESS (0x01).
  ///
  /// When active master on the bus, all incoming monitor responses are addressed to HOST_ADDRESS.
  /// Enabling host_only prevents loopback echoes or noise with broadcast/multicast addresses
  /// from being parsed.
  void set_host_only(bool host_only) { host_only_ = host_only; }

  /// @brief Whether host_only filtering is enabled.
  bool host_only() const { return host_only_; }

 private:
  /// Process an accumulated frame candidate upon encountering the 0x7E delimiter.
  void process_candidate_();

  enum class State {
    IDLE,          ///< Waiting for a 9th-bit-flagged address character.
    ACCUMULATING,  ///< Accumulating body, CRC, and delimiter bytes.
  };

  State state_{State::IDLE};
  std::vector<Uart9BitChar> buffer_;
  std::deque<Frame> frame_queue_;

  uint32_t invalid_count_{0};
  uint32_t crc_mismatch_count_{0};
  bool host_only_{false};
};

}  // namespace gensam
}  // namespace esphome

