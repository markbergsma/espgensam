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
///    - Driver Output level (dBFS, negative signed integer across active driver channels).
///    The parser handles both fixed-offset payloads (standard RACE) and tagged TLV records.
/// ===================================================================================

#include "const.h"
#include <cstdint>
#include <string>
#include <vector>
#include <map>

namespace esphome {
namespace sensor {
class Sensor;
}  // namespace sensor

namespace binary_sensor {
class BinarySensor;
}  // namespace binary_sensor

namespace text_sensor {
class TextSensor;
}  // namespace text_sensor

namespace switch_ {
class Switch;
}  // namespace switch_

namespace number {
class Number;
}  // namespace number

namespace select {
class Select;
}  // namespace select

namespace gensam {

/// @brief Static binding between a configured speaker and its ESPHome sensor entities.
struct GenSAMMonitorBinding {
  std::string name;                                    ///< Friendly speaker name (e.g. "Subwoofer", "Left Monitor").
  std::string serial_number;                           ///< Matching factory serial number (e.g. "7350AP88123456").
  uint32_t unique_id{0};                               ///< Optional matching decimal GLM hardware ID (e.g. 1842915).

  sensor::Sensor *temperature_sensor{nullptr};         ///< DSP/Amp temperature sensor.
  sensor::Sensor *input_level_sensor{nullptr};         ///< Input signal level sensor (dBFS).
  sensor::Sensor *output_level_sensor{nullptr};        ///< Driver output level sensor (dBFS).
  binary_sensor::BinarySensor *online_sensor{nullptr}; ///< Responsive online status binary sensor.
  switch_::Switch *mute_switch{nullptr};               ///< Channel mute switch entity.
  text_sensor::TextSensor *model_sensor{nullptr};       ///< Discovered model text sensor (e.g. "7350A").
  text_sensor::TextSensor *serial_sensor{nullptr};      ///< Factory serial number text sensor (e.g. "7350APM88123456").
  text_sensor::TextSensor *firmware_sensor{nullptr};    ///< Firmware revision text sensor (e.g. "1.6.2.3733").
  text_sensor::TextSensor *hardware_id_sensor{nullptr}; ///< Decimal GLM hardware ID text sensor (e.g. "1842915").
  number::Number *crossover_number{nullptr};           ///< Bass management crossover frequency number entity (Hz).
  uint16_t crossover_freq{DEFAULT_CROSSOVER_HZ};       ///< Configured / active crossover frequency in Hz (default: 85 Hz).
  bool crossover_configured{false};                    ///< True if a custom crossover was set by user or sniffed from GLM.
  select::Select *input_select{nullptr};               ///< Input routing select entity (analog / AES3 A, B, Sum).
  uint8_t source{SOURCE_ANALOG};                       ///< Configured / active input source.
  uint8_t aes3_channel{AES3_CHANNEL_A};                ///< Configured / active AES3 sub-channel; ignored when analog.

  /// True once the input has been chosen deliberately - by a group push, by Home Assistant, or
  /// snooped from GLM. While false nothing is transmitted, so a monitor keeps whatever routing
  /// its own flash holds; see the non-destructive boot note in select.h.
  bool input_configured{false};

  /// Override for the rate this speaker's PEQ bands are designed at, or 0 to derive it from
  /// the discovered model (PEQ_RATE_SUBWOOFER_HZ for a 7xxx, PEQ_RATE_DEFAULT_HZ otherwise).
  /// Only needed for a model whose rate is not yet known; see const.h.
  uint32_t peq_design_rate{0};
};

/// @brief Represents a single Genelec SAM monitor or subwoofer discovered on the RS-485 bus.
struct GenSAMMonitor {
  uint8_t address{0};                ///< Logical RS-485 bus address (e.g. 0x02, 0x03).
  uint32_t unique_id{0};             ///< Decimal GLM hardware identifier from RACE discovery (e.g. 1842915).
  std::string model;                ///< Model designation (e.g. "7350A", "8330A", "8351B").
  std::string serial_number;        ///< Factory printed serial number string (e.g. "8330AP99234567").
  std::string firmware_version;     ///< Firmware revision string (e.g. "1.6.3733").
  std::string hardware_version;     ///< Hardware revision string (e.g. "0.2.0").
  std::string raw_device_info;      ///< Full unparsed ASCII metadata string from hardware/software query.

  int8_t temperature{0};            ///< Current internal DSP/amplifier temperature in °C.
  int8_t input_db{0};               ///< Input signal level in dBFS.
  int8_t output_db{0};              ///< Driver output level in dBFS.
  bool mute{false};                 ///< Channel mute state (CMD_BYPASS bit 0).
  bool standby{true};               ///< Amplifier standby state; only meaningful when standby_known is true.
  bool standby_known{false};        ///< True once the monitor has actually reported its power state.
  bool online{false};               ///< Whether the monitor is currently responsive to bus traffic.
  uint32_t last_seen_ms{0};         ///< Timestamp (millis) of last valid frame received from this monitor.
  uint32_t last_poll_ms{0};         ///< Timestamp (millis) when the last query frame was sent to this monitor.

  GenSAMMonitorBinding *binding{nullptr}; ///< Pointer to matched Home Assistant entity binding.
  uint32_t identify_end_ms{0};            ///< If non-zero, timestamp (millis) when LED pulsing should revert.

  /// @brief Check whether this monitor is a subwoofer (7xxx series).
  /// Note: The W371 adaptive woofer system is an adaptive woofer system, not a subwoofer, and is excluded.
  bool is_subwoofer() const { return !model.empty() && model[0] == '7'; }

  /// @brief Check if this monitor matches a configured binding by serial number or unique ID.
  /// @param b The candidate binding to test against.
  /// @return True if serial number matches (case-insensitive) or decimal unique ID matches.
  bool matches(const GenSAMMonitorBinding &b) const;

  /// @brief Helper to format the decimal unique ID into a string (e.g. "1842915").
  /// @return Formatted decimal string, or "(none)" if 0.
  std::string unique_id_str() const;

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
/// Supports modern tagged TLV streams (0x41 temp, 0x42 input, 0x43-0x46 channel outputs, 0x47 power state)
/// as well as legacy fixed-offset RACE payloads.
///
/// Tag 'G' (0x47) is the only field in a reply that reports power state, and only its documented
/// values 0x01 (active) and 0x02 (standby) are accepted. A reply may also carry a 0x06/0x07 marker byte
/// at either end, or as its entire payload; see is_telemetry_marker() in const.h for what that byte is
/// and is not. Markers are stripped or skipped and never affect standby. A marker-only payload means the
/// monitor answered without reporting anything, and updates no fields.
/// @param data Pointer to raw unescaped payload buffer.
/// @param len Payload length in bytes.
/// @param[out] monitor Target monitor struct to update with temperature, signal levels, and standby state.
/// @return True if any field was parsed; false for a null/empty buffer or a marker-only payload.
bool parse_telemetry(const uint8_t *data, size_t len, GenSAMMonitor &monitor);

}  // namespace gensam
}  // namespace esphome

