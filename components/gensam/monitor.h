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
///    - Space-delimited string (e.g., reply to CMD_SOFTWARE_QUERY_SHORT 0x22):
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
///    - Modelled voice-coil temperatures per driver (tags 0x81-0x83), and on three-way models
///      the two supply-rail monitors (tags 0x88/0x89), which report a percentage of the rail's
///      span and are converted to volts with the PSU_* constants in const.h.
///    - DSP processor load (tag 0x84), a 16-bit value in per mille (Genelec GLM bus decoding
///      guide v3.0, sections 3.6 and 8). Each model's load sits in its own range, which is why
///      the high byte alone also reads as a device class (01 subwoofer, 02 two-/three-way, 03
///      transition or standby). Without signal it barely moves; it shifts briefly after a store,
///      apply or wake-up.
///    The parser handles both fixed-offset payloads (standard RACE) and tagged TLV records.
/// ===================================================================================

#include "const.h"
#include <cmath>
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
  select::Select *crossover_select{nullptr};           ///< Bass management crossover select entity (full band / Hz).
  uint16_t crossover_freq{DEFAULT_CROSSOVER_HZ};       ///< Configured / active 0x3B word: Hz, or CROSSOVER_FULL_BAND.
  bool crossover_configured{false};                    ///< True if a custom crossover was set by user or sniffed from GLM.

  number::Number *level_number{nullptr};               ///< Per-device level trim number entity (dB).
  float level_db{0.0f};                                ///< Configured / active level trim in dB; attenuation only.

  /// True once the level has been chosen deliberately, by a group push or by Home Assistant.
  /// Carries the same meaning as input_configured: while false nothing is transmitted, so the
  /// speaker keeps whatever trim its own DSP holds.
  bool level_configured{false};

  number::Number *delay_number{nullptr};               ///< Time-of-flight delay number entity (ms in HA).
  uint32_t delay_samples{0};                           ///< Configured / active delay, samples at DSP_DELAY_RATE_HZ.
  bool delay_configured{false};                        ///< True once the delay has been chosen deliberately.

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

  switch_::Switch *calibration_bypass_switch{nullptr}; ///< Per-monitor calibration bypass switch entity.

  /// True while this monitor's group calibration is bypassed on its own, whatever the hub-wide
  /// switch says; see switch.h section 5. Never restored: every boot starts calibrated.
  bool calibration_bypass{false};

  /// True for a fixed Home Assistant entity slot assigned to a monitor during discovery.
  bool discovery_slot{false};
  text_sensor::TextSensor *sam_name_sensor{nullptr}; ///< Runtime SAM setup name matched by hardware ID.

  sensor::Sensor *tw_coil_temperature_sensor{nullptr}; ///< Tweeter coil temperature sensor (°C).
  sensor::Sensor *md_coil_temperature_sensor{nullptr}; ///< Midrange coil temperature sensor (°C).
  sensor::Sensor *bs_coil_temperature_sensor{nullptr}; ///< Woofer coil temperature sensor (°C).
  sensor::Sensor *psu_main_voltage_sensor{nullptr};    ///< Main supply rail sensor (V).
  sensor::Sensor *psu_low_voltage_sensor{nullptr};     ///< Low-voltage supply rail sensor (V).
  sensor::Sensor *processor_load_sensor{nullptr};      ///< DSP processor load sensor (%).
  text_sensor::TextSensor *dip_switch_status_sensor{nullptr}; ///< Decoded current DIP switch positions.
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
  float tw_coil_temperature{NAN};   ///< Modelled tweeter voice-coil temperature (°C, tag 0x81); NAN until reported.
  float md_coil_temperature{NAN};   ///< Modelled midrange voice-coil temperature (°C, tag 0x82, three-way only).
  float bs_coil_temperature{NAN};   ///< Modelled woofer voice-coil temperature (°C, tag 0x83).
  float psu_main_voltage{NAN};      ///< Main DC supply rail (V, tag 0x88, three-way only).
  float psu_low_voltage{NAN};       ///< Low-voltage supply rail (V, tag 0x89, three-way only).
  float processor_load{NAN};        ///< DSP processor load (%, tag 0x84, firmware 1.12+).
  uint32_t dip_word{0};              ///< Current 55 84 DIP word, big-endian.
  uint32_t previous_dip_word{0};    ///< Previous 55 84 DIP word, big-endian.
  uint32_t dip_auxiliary_word{0};   ///< Third 55 84 word; meaning varies by model.
  bool dip_switches_known{false};   ///< True after a valid CMD_DIP_SWITCH 0x84 reply.
  bool mute{false};                 ///< Channel mute state (CMD_BYPASS bit 0).
  bool system_leds_enabled{true};    ///< Front LED state, when system_leds_known is true.
  bool system_leds_known{false};     ///< True once a live or stored LED setting was observed.
  bool standby{true};                ///< Amplifier standby state; only meaningful when standby_known is true.
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

/// @brief Parse ASCII device info reply from CMD_SOFTWARE_QUERY_SHORT (0x22) or CMD_SOFTWARE_QUERY (0x39).
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
/// Supports modern tagged TLV streams (0x41 temp, 0x42 input, 0x43-0x46 channel outputs, 0x47 power state,
/// 0x49 counter, 0x81-0x83 coil temperatures, 0x88/0x89 supply rails) as well as legacy fixed-offset RACE payloads.
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

/// @brief Parse the three big-endian words returned by CMD_DIP_SWITCH / DIPSW_READ_STATUS.
/// @param data Reply payload, either the 0x84 selector followed by 12 bytes or just the 12 words.
/// @param len Payload length in bytes.
/// @param[out] monitor Target monitor to update.
/// @return True when all three words were present; false for malformed input.
bool parse_dip_switch_status(const uint8_t *data, size_t len, GenSAMMonitor &monitor);

/// @brief Decode the monitor's current DIP word into switch labels for its model family.
///
/// The bit layout depends on the model (Genelec GLM bus decoding guide v3.0, sections 6.5 and
/// 6.7.1). Each switch is its own bit; paired switches whose effects add are shown as their sum.
/// - SAM monitors (8320A, 8330A, 8331A, 8340A, 8341A, 8350A, 8351A/B, 8361A, 1032C, S360A) share
///   the section 6.5 table. The guide confirms it on the 8320A/8330A software word and on the
///   8340A/8350A/8341A panels; the other models carry the same 14-switch panel in their manuals.
/// - SAM subwoofers (7350A, 7360A, 7370A, 7380A, 7382A) share the 7350A table of section 6.7.1.
///   The 7360A/7370A/7380A panels carry the same functions, but the guide has not confirmed their
///   bit positions on the bus.
/// - On the three-way monitors with a rotary level control (8331A, 8341A, 8351A/B, 8361A) the
///   third word is the knob position in 0.1 dB steps, 10 * (dB + 6).
/// - Bits 31 and 30 are the status flags of guide section 6.7.2 ("switches not valid or stored",
///   read in that order: bit 31 = not valid, bit 30 = stored), decoded on both families.
/// Bits the table does not define are shown as unknown.
/// @param monitor Monitor with a parsed DIP reply and, ideally, a discovered model.
/// @return Human-readable status, a raw word for an unrecognised model, or an empty string when
///         no DIP reply has been parsed.
std::string describe_dip_switches(const GenSAMMonitor &monitor);

}  // namespace gensam
}  // namespace esphome
