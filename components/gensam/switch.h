#pragma once

/// @file switch.h
/// @brief ESPHome Switch entity implementations for Genelec SAM mute and calibration bypass.
///
/// ===================================================================================
/// ARCHITECTURAL DESIGN RATIONALE
/// ===================================================================================
/// 1. Per-Speaker Mute Control (GenSAMMuteSwitch):
///    Genelec SAM monitors support individual acoustic muting via the proprietary RS-485
///    CMD_BYPASS (0x2B) frame directed unicast to each monitor address.
///    - When muted (state = true), bit 0 (BYPASS_MUTE_MASK) is asserted and the front LED
///      is commanded to steady RED (LED_RED << 1 = 0x02), giving clear visual feedback
///      that the monitor output is silenced.
///    - When unmuted (state = false), bit 0 is cleared and the front LED is restored
///      to normal operation (LED_OFF << 1 = 0x04).
///
/// 2. Bidirectional State Synchronization:
///    The switch reflects both locally initiated mute toggles (via Home Assistant) and
///    remote changes made through the Genelec GLM software or GLM volume controller
///    which are snooped passively on the RS-485 bus.
///
/// 3. Bypass Calibration (GenSAMCalibrationBypassSwitch, GenSAMMonitorCalibrationBypassSwitch):
///    GLM's "Calibrated" button, which reads "Cal bypassed" while pressed. Its manual (GLM 5,
///    section 6.8 and FAQ 11.4) says bypass temporarily sets the equalization, the level
///    calibration, the time-of-flight delay and the system delay to defaults that have no
///    effect. So these switches cover exactly that: all 20 PEQ slots go out as the bypass
///    vector, the level trim as 0 dB and the delay as 0 samples.
///    - Crossover, input routing and LFE are left as the group has them. Bass management is
///      GLM's separate "Bass Man" button, not part of calibration, and resetting it would
///      change which speaker plays the bass rather than only the calibration being compared.
///    - There is no opcode for it. Neither a capture nor a setup file has one, so the switch
///      re-pushes the active group with the defaults substituted. That reuses the one push
///      measured acoustically (docs/glm-group-apply.md section 8), ducks across the retune,
///      and survives standby and rediscovery, which re-push the group too.
///    - A monitor is bypassed while either the hub-wide switch or its own is on.
///    - Not restored across reboots. GLM treats bypass as temporary, and a bypass left on
///      by accident would otherwise go unnoticed after a power cut.
///    - Only groups carry calibration, so both switches exist only when groups are
///      configured. A GLM push made while bypassed replaces the defaults without this switch
///      knowing: PEQ frames are not snooped, though the level and delay snoop raises Group
///      Modified.
/// ===================================================================================

#include "esphome/core/component.h"
#include "esphome/components/switch/switch.h"
#include "hub.h"

#include <string>

namespace esphome {
namespace gensam {

/// @brief Switch entity that controls per-speaker muting and status LED state.
class GenSAMMuteSwitch : public switch_::Switch {
 public:
  /// @brief Set the parent GenSAMHub instance.
  void set_hub(GenSAMHub *hub) { hub_ = hub; }

  /// @brief Set the target speaker's factory serial number or unique ID string.
  void set_serial_or_id(const std::string &id) { serial_or_id_ = id; }

 protected:
  /// @brief Action executed when user toggles the switch in Home Assistant.
  /// @param state Target mute state (true = mute, false = unmute).
  void write_state(bool state) override {
    if (hub_ != nullptr) {
      hub_->set_monitor_mute_by_serial(serial_or_id_, state);
    }
  }

  GenSAMHub *hub_{nullptr};
  std::string serial_or_id_{};
};

/// @brief Switch entity that bypasses the active group's calibration on every monitor.
class GenSAMCalibrationBypassSwitch : public switch_::Switch {
 public:
  /// @brief Set the parent GenSAMHub instance.
  void set_hub(GenSAMHub *hub) { hub_ = hub; }

 protected:
  /// @brief Action executed when user toggles the switch in Home Assistant.
  /// @param state True to bypass the calibration, false to restore it.
  void write_state(bool state) override {
    if (hub_ != nullptr) {
      hub_->set_calibration_bypass(state);
    }
  }

  GenSAMHub *hub_{nullptr};
};

/// @brief Switch entity that bypasses the active group's calibration on one monitor.
class GenSAMMonitorCalibrationBypassSwitch : public switch_::Switch {
 public:
  /// @brief Set the parent GenSAMHub instance.
  void set_hub(GenSAMHub *hub) { hub_ = hub; }

  /// @brief Set the target speaker's factory serial number or unique ID string.
  void set_serial_or_id(const std::string &id) { serial_or_id_ = id; }

 protected:
  /// @brief Action executed when user toggles the switch in Home Assistant.
  /// @param state True to bypass this monitor's calibration, false to restore it.
  void write_state(bool state) override {
    if (hub_ != nullptr) {
      hub_->set_monitor_calibration_bypass_by_serial(serial_or_id_, state);
    }
  }

  GenSAMHub *hub_{nullptr};
  std::string serial_or_id_{};
};

}  // namespace gensam
}  // namespace esphome
