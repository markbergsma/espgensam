#pragma once

/// @file switch.h
/// @brief ESPHome Switch entity implementations for Genelec SAM per-monitor mute control.
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
/// ===================================================================================

#include "esphome/core/component.h"
#include "esphome/components/switch/switch.h"
#include "gensam_hub.h"

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

}  // namespace gensam
}  // namespace esphome
