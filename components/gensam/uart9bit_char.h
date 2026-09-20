#pragma once

/// @file uart9bit_char.h
/// @brief The 9-bit bus character, split out so the protocol layer does not depend on ESP-IDF.
///
/// Uart9BitChar is the one type the framing layer and the RMT transceiver have to agree on.
/// It used to live in uart9bit.h, which drags in driver/rmt_*.h, freertos/FreeRTOS.h and
/// soc/soc_caps.h -- so every translation unit that merely wanted to build or parse a frame
/// inherited a hard dependency on ESP-IDF, and frame.h could not be compiled by a host
/// compiler. That blocked host tests for the parts of the protocol most worth testing off
/// hardware: escaping, CRC placement, and exact wire bytes.
///
/// Giving the struct its own header costs nothing at runtime (it is a two-byte POD and the
/// constructors are trivial) and keeps the layering stated in AGENTS.md section 2 honest:
/// the protocol core owns the wire format and nothing above it owns a peripheral.

#include <cstdint>

namespace esphome {
namespace gensam {

/// @brief A single 9-bit character received from or to be transmitted on the bus.
struct Uart9BitChar {
  uint8_t data;       ///< 8-bit data value (D0–D7).
  uint8_t ninth_bit;  ///< 9th bit: 1 = address byte, 0 = data byte.

  Uart9BitChar() : data(0), ninth_bit(0) {}
  Uart9BitChar(uint8_t d, uint8_t n) : data(d), ninth_bit(n) {}
};

}  // namespace gensam
}  // namespace esphome
