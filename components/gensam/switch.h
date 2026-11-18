#pragma once

/// @file switch.h
/// @brief ESPHome Switch entities for Genelec SAM system power, mute, monitor LEDs, and calibration
///        and phase-correction bypass.
///
/// ===================================================================================
/// ARCHITECTURAL DESIGN RATIONALE
/// ===================================================================================
/// 1. System Power (GenSAMSystemPowerSwitch):
///    Exposes the hub's active/standby state as a conventional power switch: on means awake,
///    off means amplifier standby. State follows both local commands and passively observed
///    power changes.
///
/// 2. Per-Speaker Mute Control (GenSAMMuteSwitch):
///    Genelec SAM monitors support individual acoustic muting via the proprietary RS-485
///    CMD_BYPASS (0x2B) frame directed unicast to each monitor address.
///    - When muted (state = true), bit 0 (BYPASS_MUTE_MASK) is asserted and the front LED
///      is commanded to steady RED (LED_RED << 1 = 0x02), giving clear visual feedback
///      that the monitor output is silenced.
///    - When unmuted (state = false), bit 0 is cleared and the front LED is restored
///      to normal operation (LED_OFF << 1 = 0x04).
///
/// 3. Bidirectional State Synchronization:
///    The switch reflects both locally initiated mute toggles (via Home Assistant) and
///    remote changes made through the Genelec GLM software or GLM volume controller
///    which are snooped passively on the RS-485 bus.
///
/// 4. Monitor LEDs (GenSAMSystemLEDSwitch):
///    Controls the live front-LED state using CMD_BYPASS (0x2B), independently of the stored ISS
///    LED preference. GLM's live 0x2B frames and stored 0x3A 06 frames both update the switch.
///
/// 5. Bypass Calibration (GenSAMCalibrationBypassSwitch, GenSAMMonitorCalibrationBypassSwitch):
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
///
/// 6. Bypass Phase Correction (GenSAMPhaseCorrectionBypassSwitch):
///    Bypasses the speakers' group-delay (phase-correction) FIR filter with CMD_BYPASS_FILTERS
///    0x53 00 01, and restores it with 0x53 00 00 (GLM bus decoding guide v3.0, section 10.3).
///    Unlike calibration bypass this is a real opcode, so nothing is re-pushed. The guide's
///    other mask, 02, bypasses every remaining audio filter and is deliberately not offered.
///    The guide's frames were captured on one speaker; whether every SAM model honours the
///    command has not been verified on this bus.
///    Not restored across reboots, for the same reason as calibration bypass.
/// ===================================================================================

#include "esphome/core/component.h"
#include "esphome/components/switch/switch.h"
#include "hub.h"

#include <string>

namespace esphome {
namespace gensam {

/// @brief Switch controlling system power; on means the monitors are awake.
class GenSAMSystemPowerSwitch : public switch_::Switch {
 public:
  /// @brief Register the hub and synchronize with its reported power state.
  /// @param hub Parent hub.
  void set_hub(GenSAMHub *hub) {
    hub_ = hub;
    hub_->add_state_callback([this](float, bool, bool standby) { this->publish_state(!standby); });
    this->publish_state(!hub_->is_standby());
  }

 protected:
  void write_state(bool state) override {
    if (hub_ != nullptr) {
      hub_->set_standby(!state);
      this->publish_state(!hub_->is_standby());
    }
  }

  GenSAMHub *hub_{nullptr};
};

/// @brief Switch controlling system-wide mute through the hub's deferred write path.
class GenSAMSystemMuteSwitch : public switch_::Switch {
 public:
  /// @brief Register the hub and synchronize with its reported mute state.
  /// @param hub Parent hub.
  void set_hub(GenSAMHub *hub) {
    hub_ = hub;
    hub_->add_state_callback([this](float, bool muted, bool) { this->publish_state(muted); });
  }

 protected:
  void write_state(bool state) override {
    if (hub_ != nullptr) {
      hub_->set_group_mute(state);
      this->publish_state(hub_->is_muted());
    }
  }

  GenSAMHub *hub_{nullptr};
};

/// @brief Switch controlling whether the monitors' front LEDs are enabled.
class GenSAMSystemLEDSwitch : public switch_::Switch {
 public:
  /// @brief Register the hub and synchronize with its known LED setting.
  /// @param hub Parent hub.
  void set_hub(GenSAMHub *hub) {
    hub_ = hub;
    hub_->add_system_led_state_callback([this](bool enabled) { this->publish_state(enabled); });
    if (hub_->is_system_led_state_known()) {
      this->publish_state(hub_->are_system_leds_enabled());
    }
  }

 protected:
  void write_state(bool state) override {
    if (hub_ != nullptr) {
      hub_->set_system_leds_enabled(state);
    }
  }

  GenSAMHub *hub_{nullptr};
};

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

/// @brief Switch entity that bypasses the phase-correction filter on every monitor.
class GenSAMPhaseCorrectionBypassSwitch : public switch_::Switch {
 public:
  /// @brief Set the parent GenSAMHub instance.
  void set_hub(GenSAMHub *hub) { hub_ = hub; }

 protected:
  /// @brief Action executed when user toggles the switch in Home Assistant.
  /// @param state True to bypass phase correction, false to restore it.
  void write_state(bool state) override {
    if (hub_ != nullptr) {
      hub_->set_phase_correction_bypass(state);
    }
  }

  GenSAMHub *hub_{nullptr};
};

}  // namespace gensam
}  // namespace esphome
