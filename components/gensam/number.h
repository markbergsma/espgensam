#pragma once

/// @file number.h
/// @brief ESPHome Number entity implementation for Genelec SAM bass management crossover frequency control.
///
/// ===================================================================================
/// ARCHITECTURAL DESIGN RATIONALE
/// ===================================================================================
/// 1. Bass Management Crossover Control (GenSAMCrossoverNumber):
///    Genelec SAM subwoofers and monitors support electronic bass management crossover
///    configuration via the RS-485 opcode CMD_BASS_MANAGE_XO (0x3B).
///    - The payload is a 2-byte big-endian unsigned integer representing the crossover
///      filter frequency in Hertz (e.g., 0x0055 = 85 Hz, 0x005A = 90 Hz).
///    - Genelec GLM enforces 5 Hz quantization increments across a typical operational range
///      of 50 Hz to 120 Hz, with 85 Hz serving as the acoustic factory default.
///    - The control() implementation rounds requested changes to the nearest 5 Hz step
///      and clamps values strictly within the [50, 120] Hz bounds to maintain DSP filter
///      stability and prevent out-of-spec filter coefficient calculation.
///
/// 2. Non-Destructive Startup & Stored Preset Preservation:
///    - Monitors reply with NACK (0x11) when queried with empty 0x3B frames; stored filter
///      frequencies cannot be read back over the wire.
///    - To avoid overwriting the speaker's internal flash presets on boot with an arbitrary default,
///      the entity starts in an unconfigured state (state = NAN / Unknown in Home Assistant).
///    - The hub suppresses transmission of CMD_BASS_MANAGE_XO during initial boot and discovery.
///
/// 3. Bidirectional Synchronization & Volatile Persistence:
///    - When adjusted via Home Assistant, the entity dispatches the unicast frame to the
///      monitor, marks the binding configured, and publishes state.
///    - Monitor crossover settings are stored in volatile RAM bindings and re-transmitted
///      automatically during the bus configuration phase (CONFIGURING_DEVICES) whenever
///      monitors are woken up from standby or rediscovered.
///    - In addition, passive bus sniffing detects GLM-initiated changes on the RS-485 bus,
///      ensuring Home Assistant reflects adjustments made in official Genelec software.
/// ===================================================================================

#include "esphome/core/component.h"
#include "esphome/components/number/number.h"
#include "gensam_hub.h"
#include "const.h"

#include <cmath>
#include <string>

namespace esphome {
namespace gensam {

/// @brief Number entity that configures the bass management crossover frequency (Hz) for a SAM speaker.
class GenSAMCrossoverNumber : public number::Number {
 public:
  /// @brief Set the parent GenSAMHub instance.
  /// @param hub Pointer to the GenSAMHub.
  void set_hub(GenSAMHub *hub) { hub_ = hub; }

  /// @brief Set the target speaker's factory serial number or unique ID string.
  /// @param id Serial number string or unique ID string.
  void set_serial_or_id(const std::string &id) { serial_or_id_ = id; }

 protected:
  /// @brief Action executed when user adjusts the number value in Home Assistant.
  /// @param value Requested crossover frequency in Hz.
  void control(float value) override {
    float step = static_cast<float>(CROSSOVER_STEP_HZ);
    float rounded = std::round(value / step) * step;
    if (rounded < MIN_CROSSOVER_HZ) {
      rounded = static_cast<float>(MIN_CROSSOVER_HZ);
    }
    if (rounded > MAX_CROSSOVER_HZ) {
      rounded = static_cast<float>(MAX_CROSSOVER_HZ);
    }
    uint16_t freq = static_cast<uint16_t>(rounded);

    if (hub_ != nullptr) {
      hub_->set_monitor_crossover_by_serial(serial_or_id_, freq);
    }
    this->publish_state(rounded);
  }

  GenSAMHub *hub_{nullptr};        ///< Pointer to root GenSAM bus controller hub.
  std::string serial_or_id_{};    ///< Target monitor serial number or decimal ID.
};

}  // namespace gensam
}  // namespace esphome
