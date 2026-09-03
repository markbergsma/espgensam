#pragma once

/// @file monitor.h
/// @brief Data structures and parsers for discovered Genelec SAM monitors.
///
/// ===================================================================================
/// ARCHITECTURE & DESIGN RATIONALE
/// ===================================================================================
/// Genelec Smart Active Monitors (SAM) and subwoofers (e.g. 8320A, 8330A, 8351B, 7350A)
/// communicate configuration and runtime telemetry through structured ASCII and binary
/// frames.
///
/// 1. Device Identification & Metadata Parsing:
///    During discovery and device interrogation, monitors report hardware and software
///    identity strings using two distinct formats:
///    - Semicolon-delimited key-value string (e.g., from software queries or modern firmware):
///        "c-1;model-7350A;ver-1.6.2.3733;hw-0.0.0;build-..."
///      Extracted keys:
///        model-      -> Model designation (e.g., "7350A", "8330A")
///        ver-        -> Firmware revision string
///        hw-         -> Hardware board revision
///    - Space-delimited string (e.g., reply to CMD_HARDWARE_QUERY 0x22):
///        "7350A 1 0000 0106 3733 "
///      Tokens: [Model, HardwareType, HardwareConfig, VersionCode, BuildNumber]
///      where VersionCode (e.g., 0106) encodes Major (01) and Minor (06) firmware revision.
///
/// 2. Telemetry & Acoustic Metrics:
///    Live monitor state is obtained by periodic status polling (CMD_QUERY_STATUS 0x08 /
///    CMD_REPORT_STATUS 0x09). The binary telemetry payload encodes:
///    - Internal amplifier / DSP temperature (degrees Celsius).
///    - Analog/Digital Input level (dBFS, negative signed integer).
///    - Driver Output level (dBFS, negative signed integer).
///    - Fault, clip, and protection states.
///    The parser handles both fixed-offset payloads (standard RACE) and tagged TLV records.
/// ===================================================================================

#include <cstdint>
#include <string>
#include <vector>
#include <map>

namespace esphome {
namespace gensam {

/// @brief Represents a single Genelec SAM monitor or subwoofer discovered on the RS-485 bus.
struct GenSAMMonitor {
  uint8_t address{0};                ///< Logical RS-485 bus address (e.g. 0x02, 0x03).
  std::vector<uint8_t> unique_id;   ///< 3-byte hardware serial/MAC identifier from RACE discovery.
  std::string model;                ///< Model designation (e.g. "7350A", "8330A", "8351B").
  std::string serial_number;        ///< Factory printed serial number string (e.g. "8330AP61020259").
  std::string firmware_version;     ///< Firmware revision string (e.g. "1.6.3733").
  std::string hardware_version;     ///< Hardware revision string (e.g. "0.2.0").
  std::string raw_device_info;      ///< Full unparsed ASCII metadata string from hardware/software query.

  int8_t temperature{0};            ///< Current internal DSP/amplifier temperature in °C.
  int8_t input_db{0};               ///< Input signal level in dBFS.
  int8_t output_db{0};              ///< Driver output level in dBFS.
  bool clip{false};                 ///< Limiter or overload protection state.
  bool online{false};               ///< Whether the monitor is currently responsive to bus traffic.
  uint32_t last_seen_ms{0};         ///< Timestamp (millis) of last valid frame received from this monitor.
  uint32_t last_poll_ms{0};         ///< Timestamp (millis) when the last query frame was sent to this monitor.

  /// @brief Helper to format the 3-byte unique ID into a space-separated hex string (e.g. "11 3E 49").
  /// @return Formatted hex string, or "(none)" if empty.
  std::string unique_id_hex() const;

  /// @brief Formatted summary string for logging and diagnostic dumps.
  /// @return Human-readable summary of monitor address, ID, serial number, model, firmware, and online status.
  std::string to_string() const;
};

/// @brief Parse ASCII device info reply from CMD_HARDWARE_QUERY (0x22) or CMD_SOFTWARE_QUERY (0x39).
/// @param data Pointer to raw unescaped payload buffer.
/// @param len Payload length in bytes.
/// @param[out] monitor Target monitor struct to populate with model, firmware, and hardware versions.
/// @return True if model was successfully identified, false otherwise.
bool parse_device_info(const uint8_t *data, size_t len, GenSAMMonitor &monitor);

/// @brief Parse ASCII factory barcode / serial number reply from CMD_BAR_CODE (0x19).
/// @param data Pointer to raw unescaped payload buffer.
/// @param len Payload length in bytes.
/// @param[out] monitor Target monitor struct to populate with serial_number string.
/// @return True if serial number string was non-empty and parsed, false otherwise.
bool parse_barcode(const uint8_t *data, size_t len, GenSAMMonitor &monitor);

/// @brief Parse status telemetry payload from CMD_REPORT_STATUS (0x09) or CMD_QUERY_STATUS (0x08).
///
/// Supports modern tagged TLV streams ('A' temp, 'B' input, 'C' clip, 'E'/'F' output)
/// as well as legacy fixed-offset RACE payloads.
/// @param data Pointer to raw unescaped payload buffer.
/// @param len Payload length in bytes.
/// @param[out] monitor Target monitor struct to update with temperature, signal levels, and clip state.
/// @return True if telemetry was successfully parsed, false if buffer is null/empty.
bool parse_telemetry(const uint8_t *data, size_t len, GenSAMMonitor &monitor);

}  // namespace gensam
}  // namespace esphome

