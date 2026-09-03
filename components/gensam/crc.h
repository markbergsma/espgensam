#pragma once

/// @file crc.h
/// @brief CRC-16/GSM Checksum computation for Genelec SAM GLM RS-485 bus frames.
///
/// ===================================================================================
/// ARCHITECTURE & DESIGN RATIONALE
/// ===================================================================================
/// The Genelec GLM protocol employs a 16-bit Cyclic Redundancy Check (CRC) to guarantee
/// data integrity across multidrop RS-485 cable runs.
///
/// Parameters of the CRC-16/GSM specification:
/// - Generator Polynomial: 0x1021 (x^16 + x^12 + x^5 + 1, normal CCITT form)
/// - Initial Value:        0x0000
/// - Final XOR Value:      0xFFFF
/// - Input Reflected:      False (bytes processed MSB-first)
/// - Result Reflected:     False (CRC output MSB-first)
///
/// Scope of Checksum:
/// The CRC is computed over the unescaped sequence:
///   [Address (1 byte) + Command (1 byte) + Payload (0 to N bytes)]
///
/// During transmission (TX), the 16-bit CRC is calculated over raw bytes, and its two
/// bytes (MSB first) are appended to the body before byte-stuffing/escaping is applied.
/// During reception (RX), after the parser unescapes the body and CRC bytes, this function
/// validates that the payload CRC matches the calculated checksum.
/// ===================================================================================

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace gensam {

/// @brief Calculate CRC-16/GSM over a buffer of bytes using a precomputed lookup table.
/// @param data Pointer to input data buffer.
/// @param len Number of bytes in @p data.
/// @return Computed 16-bit CRC value.
uint16_t calculate_crc(const uint8_t *data, size_t len);

/// @brief Verify if the calculated CRC-16/GSM matches an expected 16-bit wire value.
/// @param data Pointer to input data buffer.
/// @param len Number of bytes in @p data.
/// @param expected_crc The 16-bit CRC received over the wire.
/// @return True if computed CRC matches @p expected_crc, false otherwise.
bool verify_crc(const uint8_t *data, size_t len, uint16_t expected_crc);

}  // namespace gensam
}  // namespace esphome
